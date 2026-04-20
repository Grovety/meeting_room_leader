#include "board/esp_panel_board_default_config.hpp"
#include "cJSON.h"
#include "config_portal.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_display_panel.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "language_manager.h"
#include "location_service.h"
#include "lvgl_v8_port.h"
#include "nvs_flash.h"
#include "port/esp_io_expander.h"
#include "port/esp_io_expander_tca9554.h"
#include "sdkconfig.h"
#include "sleep_manager.h"
#include "time_sync.h"
#include "weather_integration.h"
#include "wifi_manager.h"
#include "aux_udp_link.h"
#include <lvgl.h>
extern "C" {
#include "ui.h"
void ui_update_time_display(uint8_t hour, uint8_t minute);
/* Initialization (optional, but provides a unified interface) */
esp_err_t rtc_ds3231_init(void);

/* Read epoch from RTC. Returns true on success and if the time is valid (> 2000-01-01). */
bool rtc_ds3231_read_epoch(time_t* out_epoch);

/* Write epoch to RTC (local time). Returns true on success. */
bool rtc_ds3231_write_epoch(time_t epoch);
}
#include "esp_event.h"
#include "esp_netif.h"
#include "i2c_master.h" // assuming this provides i2c_master_init()

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static const char* TAG = "app";
#define APP_HEAP_LOGGING_ENABLED 0

/* UART settings */
#define GUI_UART_PORT   UART_NUM_0 // USB-Serial used by PC GUI (app.py)
#define GUI_RX_BUF_SIZE 1024

/* I2C settings for hardware version detection */
#define I2C_MASTER_TIMEOUT_MS 1000
#define I2C_PORT_NUM          I2C_NUM_0
#define BACKLIGHT_ADDR_V1_1   0x30
#define BACKLIGHT_ADDR_V1_0   0x18

static double gl_lat     = 0.0;
static double gl_lon     = 0.0;
static char gl_city[100] = "";
static char gl_tz[64]    = "";
static volatile bool s_wifi_init_done = false;

#if APP_HEAP_LOGGING_ENABLED
static const char* TAG_PSRAM = "psram";
static const char* TAG_INTERNAL_HEAP = "internal_heap";

static void log_psram_heap_stats(const char* label)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);

    unsigned long long free_bytes     = (unsigned long long)info.total_free_bytes;
    unsigned long long used_bytes     = (unsigned long long)info.total_allocated_bytes;
    unsigned long long total_bytes    = free_bytes + used_bytes;
    unsigned long long largest_bytes  = (unsigned long long)info.largest_free_block;
    unsigned long long min_free_bytes = (unsigned long long)info.minimum_free_bytes;

    ESP_LOGI(
        TAG_PSRAM,
        "%s: total=%llu KiB free=%llu KiB used=%llu KiB largest=%llu KiB min_free=%llu KiB blocks alloc/free=%llu/%llu",
        label ? label : "PSRAM", total_bytes / 1024ULL, free_bytes / 1024ULL, used_bytes / 1024ULL,
        largest_bytes / 1024ULL, min_free_bytes / 1024ULL, (unsigned long long)info.allocated_blocks,
        (unsigned long long)info.free_blocks);
}

static void log_internal_heap_stats(const char* label)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    unsigned long long free_bytes     = (unsigned long long)info.total_free_bytes;
    unsigned long long used_bytes     = (unsigned long long)info.total_allocated_bytes;
    unsigned long long total_bytes    = free_bytes + used_bytes;
    unsigned long long largest_bytes  = (unsigned long long)info.largest_free_block;
    unsigned long long min_free_bytes = (unsigned long long)info.minimum_free_bytes;

    ESP_LOGI(
        TAG_INTERNAL_HEAP,
        "%s: total=%llu KiB free=%llu KiB used=%llu KiB largest=%llu KiB min_free=%llu KiB blocks alloc/free=%llu/%llu",
        label ? label : "INTERNAL heap", total_bytes / 1024ULL, free_bytes / 1024ULL, used_bytes / 1024ULL,
        largest_bytes / 1024ULL, min_free_bytes / 1024ULL, (unsigned long long)info.allocated_blocks,
        (unsigned long long)info.free_blocks);
}

static void log_heap_checkpoint(const char* stage)
{
    log_internal_heap_stats(stage ? stage : "heap checkpoint");
    log_psram_heap_stats(stage ? stage : "heap checkpoint");
}

static void psram_monitor_task(void* arg)
{
    (void)arg;
    log_psram_heap_stats("PSRAM heap");
    log_internal_heap_stats("INTERNAL heap");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        log_psram_heap_stats("PSRAM heap");
        log_internal_heap_stats("INTERNAL heap");
    }
}
#else
static void log_heap_checkpoint(const char* stage)
{
    (void)stage;
}
#endif

/* Forward declarations */
void network_sync_task(void* arg);
static esp_panel::board::Board* create_board_for_hw(bool is_v1_1);
static BaseType_t start_network_sync_task(void);
static void apply_runtime_log_policy(void);

static void apply_runtime_log_policy(void)
{
    // Reduce runtime UART/log pressure from high-frequency modules.
    // This helps avoid UI jitter under heavy voice/network workloads.
    esp_log_level_set("weather_service", ESP_LOG_WARN);
}

/* Low-level I2C helpers (probe/write) */
static esp_err_t native_i2c_write_byte(uint8_t dev_addr, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = ESP_ERR_TIMEOUT;
    if (i2c_master_lock(I2C_MASTER_TIMEOUT_MS)) {
        ret = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        i2c_master_unlock();
    }
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t native_i2c_probe(uint8_t dev_addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = ESP_ERR_TIMEOUT;
    if (i2c_master_lock(I2C_MASTER_TIMEOUT_MS)) {
        ret = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        i2c_master_unlock();
    }
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* Optional I2C scan (for debug) */
static void i2c_scan_once(void)
{
    ESP_LOGI(TAG, "I2C scan start...");
    if (!i2c_master_lock(5000)) {
        ESP_LOGW(TAG, "I2C scan skipped: failed to lock bus");
        return;
    }
    for (int addr = 1; addr < 0x7f; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (! cmd) {
            ESP_LOGW(TAG, "I2C scan aborted: failed to allocate command link");
            break;
        }
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
        }
    }
    i2c_master_unlock();
    ESP_LOGI(TAG, "I2C scan done");
}

/* --------------------------- UART0 task: JSON parser for GUI (same logic as we tested earlier)
   Reads from GUI_UART_PORT (USB -> PC) and handles runtime JSON messages
   --------------------------- */
static void gui_uart_task(void* arg)
{
    (void)arg;
    uint8_t* data = (uint8_t*)malloc(GUI_RX_BUF_SIZE);
    if (! data) {
        ESP_LOGE(TAG, "gui_uart_task: malloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "gui_uart_task started, reading from UART port %d", (int)GUI_UART_PORT);

    while (1) {
        int len = uart_read_bytes(GUI_UART_PORT, data, GUI_RX_BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = 0;
            char* s   = (char*)data;
            // trim leading CR/LF
            while (*s && (*s == '\r' || *s == '\n'))
                ++s;
            if (*s == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            ESP_LOGI(TAG, "UART0 RX (%d bytes)", len);

            cJSON* root = cJSON_Parse(s);
            if (root) {
                cJSON* ssid = cJSON_GetObjectItem(root, "wifi_ssid");
                cJSON* pass = cJSON_GetObjectItem(root, "wifi_pass");

                if (cJSON_IsString(ssid) && cJSON_IsString(pass)) {
                    ESP_LOGI(TAG, "UART0: got wifi credentials (ssid='%s')", ssid->valuestring);
                    wifi_manager_save_credentials(ssid->valuestring, pass->valuestring);
                    wifi_manager_init_sta(ssid->valuestring, pass->valuestring);
                    const char* ok = "WIFI:OK\n";
                    uart_write_bytes(GUI_UART_PORT, ok, strlen(ok));
                } else {
                    ESP_LOGW(TAG, "UART0: JSON received but no known fields (wifi_ssid/wifi_pass)");
                }

                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "UART0: failed to parse as JSON");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(data);
    vTaskDelete(NULL);
}

/* --------------------------- network_sync_task area --------------------------- */
static TaskHandle_t s_network_sync_task_handle = NULL;
static StaticTask_t s_network_sync_tcb;
static StackType_t* s_network_sync_stack       = nullptr;
static bool s_network_sync_stack_in_psram      = false;

static bool wifi_matches_saved_ssid(const char* ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    wifi_ap_record_t ap_info = {};
    if (wifi_manager_lock(250)) {
        if (wifi_manager_is_connected() && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (strncmp((const char*)ap_info.ssid, ssid, sizeof(ap_info.ssid)) == 0) {
                wifi_manager_unlock();
                return true;
            }
        }

        wifi_config_t current_cfg = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK) {
            if (strncmp((const char*)current_cfg.sta.ssid, ssid, sizeof(current_cfg.sta.ssid)) == 0) {
                wifi_manager_unlock();
                return true;
            }
        }

        wifi_manager_unlock();
    }

    return false;
}

static BaseType_t start_network_sync_task(void)
{
    constexpr size_t kNetworkSyncStackBytes = 8 * 1024;

    if (s_network_sync_task_handle != NULL) {
        return pdPASS;
    }

    if (!s_network_sync_stack) {
        void* stack_ptr = nullptr;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
        stack_ptr = heap_caps_malloc(kNetworkSyncStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stack_ptr) {
            s_network_sync_stack_in_psram = true;
        }
#endif
        if (!stack_ptr) {
            stack_ptr = heap_caps_malloc(kNetworkSyncStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            s_network_sync_stack_in_psram = false;
        }
        if (!stack_ptr) {
            ESP_LOGE(TAG, "Failed to allocate stack for network_sync_task (%u bytes)",
                     (unsigned)kNetworkSyncStackBytes);
            return pdFAIL;
        }
        s_network_sync_stack = static_cast<StackType_t*>(stack_ptr);
    }

    TaskHandle_t handle = xTaskCreateStatic(
        network_sync_task,
        "net_sync",
        kNetworkSyncStackBytes / sizeof(StackType_t),
        NULL,
        5,
        s_network_sync_stack,
        &s_network_sync_tcb);
    if (!handle) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for network_sync_task");
        return pdFAIL;
    }

    s_network_sync_task_handle = handle;
    ESP_LOGI(TAG, "network_sync task created with stack=%u bytes (%s)",
             (unsigned)kNetworkSyncStackBytes,
             s_network_sync_stack_in_psram ? "PSRAM" : "INTERNAL");
    return pdPASS;
}

static esp_panel::board::Board* create_board_for_hw(bool is_v1_1)
{
    (void)is_v1_1;
    return new Board();
}

/* network_sync_task (keeps same logic) */
void network_sync_task(void* arg)
{
    (void)arg;
    const int loc_max_attempts = 10;
    const int loc_delay_ms     = 2000;
    while (1) {
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "[1/4] WiFi Connected. Starting IP Location...");
            bool loc_ok = false;
            for (int i = 0; i < loc_max_attempts; ++i) {
                if (location_service_get_by_ip()) {
                    if (location_service_get(&gl_lat, &gl_lon, gl_city, sizeof(gl_city))) {
                        if (location_service_get_timezone(gl_tz, sizeof(gl_tz))) {
                            ESP_LOGI(TAG, "[2/4] Location obtained: %s (%f, %f), TZ: %s", gl_city, gl_lat, gl_lon,
                                     gl_tz);
                            loc_ok = true;
                            break;
                        } else {
                            ESP_LOGW(TAG, "Location obtained but timezone missing, attempting detect by city...");
                            if (strlen(gl_city) > 0) {
                                const char* tz_by_city = location_service_detect_timezone_by_city(gl_city);
                                if (tz_by_city && strlen(tz_by_city) > 0) {
                                    strncpy(gl_tz, tz_by_city, sizeof(gl_tz) - 1);
                                    gl_tz[sizeof(gl_tz) - 1] = '\0';
                                    ESP_LOGI(TAG, "Detected TZ by city: %s", gl_tz);
                                    loc_ok = true;
                                    break;
                                }
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "location_service_get returned not valid cached data despite HTTP success");
                    }
                } else {
                    ESP_LOGW(TAG, "location_service_get_by_ip attempt %d failed, retrying...", i + 1);
                }
                vTaskDelay(pdMS_TO_TICKS(loc_delay_ms));
            }

            if (! loc_ok) {
                ESP_LOGW(TAG,
                         "Failed to obtain location after retries — will use fallback: Paris (for time & weather)");
                location_service_set_manual(48.8566, 2.3522, "Paris");
                gl_lat = 48.8566;
                gl_lon = 2.3522;
                strncpy(gl_city, "Paris", sizeof(gl_city) - 1);
                gl_city[sizeof(gl_city) - 1] = '\0';
                strncpy(gl_tz, "Europe/Paris", sizeof(gl_tz) - 1);
                gl_tz[sizeof(gl_tz) - 1] = '\0';
                loc_ok                   = true;
            }

            if (loc_ok) {
                ESP_LOGI(TAG, "[3/4] Starting SNTP sync...");
                if (time_sync_start(gl_tz, gl_city)) {
                    int wait_seconds = 30;
                    int waited       = 0;
                    while (waited++ < wait_seconds) {
                        if (time_sync_is_synced()) {
                            ESP_LOGI(TAG, "Time synchronized successfully!");
                            break;
                        }
                        ESP_LOGI(TAG, "Waiting for system clock sync... (%d/%d)", waited, wait_seconds);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }

                    if (! time_sync_is_synced()) {
                        ESP_LOGW(TAG, "Time not synchronized within timeout; weather requests may be unreliable.");
                    }
                } else {
                    ESP_LOGE(TAG, "SNTP task did not start; continuing without confirmed network time");
                }

                ESP_LOGI(TAG, "[4/4] Starting weather module for %s...", gl_city);
                weather_integration_set_location(gl_lat, gl_lon, gl_city);
                weather_integration_start();
                log_heap_checkpoint("heap[net_sync: after weather start]");

                ESP_LOGI(TAG, "Full network synchronization complete.");

                vTaskDelay(pdMS_TO_TICKS(500));
                ESP_LOGI(TAG, "Network sync task finished.");
                s_network_sync_task_handle = NULL;
                vTaskDelete(NULL);
            } else {
                ESP_LOGW(TAG, "Unexpected state: loc_ok == false after fallback handling.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Deferred WiFi init task (UI-first) */
static void wifi_init_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wifi_init_task: starting Wi-Fi initialization (deferred)");
    log_heap_checkpoint("heap[wifi_init: enter]");

    char s_ssid[33] = {0}, s_pass[65] = {0};
    esp_err_t fetch_err = wifi_manager_fetch_credentials(s_ssid, s_pass);
    if (fetch_err == ESP_OK && strlen(s_ssid) > 0) {
        if (wifi_matches_saved_ssid(s_ssid)) {
            ESP_LOGI(TAG, "Wi-Fi already configured for saved SSID: %s; skipping deferred reconnect", s_ssid);
        } else {
            ESP_LOGI(TAG, "Stored WiFi found: %s (deferred init)", s_ssid);
            esp_err_t init_ret = wifi_manager_init_sta(s_ssid, s_pass);
            ESP_LOGI(TAG, "wifi_manager_init_sta returned %s", esp_err_to_name(init_ret));
        }
    } else {
        ESP_LOGW(TAG, "No WiFi stored. Initializing manager (no credentials) — GUI config possible");
        esp_err_t init_ret = wifi_manager_init_sta("", "");
        ESP_LOGI(TAG, "wifi_manager_init_sta (empty) returned %s", esp_err_to_name(init_ret));
    }
    log_heap_checkpoint("heap[wifi_init: after wifi init]");

    BaseType_t r = start_network_sync_task();
    ESP_LOGI(TAG, "network_sync task create result: %d", (int)r);
    log_heap_checkpoint("heap[wifi_init: after net_sync create]");

    s_wifi_init_done = true;
    vTaskDelete(NULL);
}

/* --------------------------- app_main --------------------------- */
extern "C" void app_main()
{
    ESP_LOGI(TAG, "app_main starting...");
    log_heap_checkpoint("heap[startup: enter app_main]");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG,
                 "NVS initialization failed with %s. Automatic erase is disabled to preserve user data; "
                 "manual migration or erase is required.",
                 esp_err_to_name(ret));
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
    }

    language_init(true);
    time_sync_apply_saved_timezone();
    apply_runtime_log_policy();
    log_heap_checkpoint("heap[startup: after base init]");

    const bool boot_into_config_portal = config_portal_consume_boot_request();
    ESP_LOGI(TAG, "Boot mode: %s", boot_into_config_portal ? "one-shot setup portal" : "normal");

    /* --- Early I2C initialization: critical before Board init for v1.0 expander --- */
    ESP_LOGI(TAG, "Initializing I2C master (required for IO expanders)...");
    extern esp_err_t i2c_master_init(void);
    if (i2c_master_init() != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_init() returned error (continuing).");
    } else {
        ESP_LOGI(TAG, "i2c_master_init() succeeded");
    }

    /* Optional scan for debug */
    i2c_scan_once();
    // After i2c_scan_once();
    ESP_LOGI(TAG, "Initializing RTC (if present)...");
    if (rtc_ds3231_init() == ESP_OK) {
        time_t rtc_t;
        if (rtc_ds3231_read_epoch(&rtc_t)) {
            struct timeval tv = {.tv_sec = rtc_t, .tv_usec = 0};
            if (settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "System time preset from RTC: %s", ctime(&rtc_t));
            } else {
                ESP_LOGW(TAG, "Failed to set system time from RTC at startup");
            }
        } else {
            ESP_LOGI(TAG, "RTC present but returned invalid time");
        }
    } else {
        ESP_LOGI(TAG, "No RTC detected (rtc_ds3231_init failed)");
    }

    /* Probe for hardware version: v1.1 (0x30) or v1.0 (expander 0x18) */
    bool is_v1_1 = false;
    int retries  = 3;
    ESP_LOGI(TAG, "Probing for backlight controller at 0x30 (v1.1)...");
    while (retries-- > 0) {
        esp_err_t probe_err = native_i2c_probe(BACKLIGHT_ADDR_V1_1);
        if (probe_err == ESP_OK) {
            ESP_LOGI(TAG, "Backlight controller 0x30 found -> hardware v1.1 likely.");
            esp_err_t write_err = native_i2c_write_byte(BACKLIGHT_ADDR_V1_1, 0x10);
            if (write_err == ESP_OK) {
                ESP_LOGI(TAG, "Sent enable command to 0x30 (ok).");
            } else {
                ESP_LOGW(TAG, "Send to 0x30 returned %s", esp_err_to_name(write_err));
            }
            is_v1_1 = true;
            break;
        } else {
            ESP_LOGI(TAG, "0x30 not present (attempts left: %d)", retries);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    esp_io_expander_handle_t manual_expander = NULL;

    if (! is_v1_1) {
        ESP_LOGI(TAG, "No 0x30 found -> likely hardware v1.0. Attempt manual TCA9554 init at 0x18...");

        esp_err_t err = ESP_ERR_TIMEOUT;
        if (i2c_master_lock(1000)) {
            err = esp_io_expander_new_i2c_tca9554(I2C_PORT_NUM, BACKLIGHT_ADDR_V1_0, &manual_expander);
            i2c_master_unlock();
        }
        if (err == ESP_OK && manual_expander) {
            ESP_LOGI(TAG, "Manual TCA9554 handle created for 0x18.");

            // Configure pins and switch levels for backlight / reset (adapted from examples)
            uint32_t output_pins = BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(7);

            if (i2c_master_lock(1000)) {
                esp_io_expander_set_dir(manual_expander, output_pins, IO_EXPANDER_OUTPUT);
                esp_io_expander_set_level(manual_expander, BIT(1) | BIT(7) | BIT(3), 1);
                i2c_master_unlock();
            }

            // Reset pulse (if using GPIO1 through expander or direct)
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_direction(GPIO_NUM_1, GPIO_MODE_OUTPUT);
            gpio_set_level(GPIO_NUM_1, 0);
            if (i2c_master_lock(1000)) {
                esp_io_expander_set_level(manual_expander, BIT(2), 0);
                i2c_master_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            if (i2c_master_lock(1000)) {
                esp_io_expander_set_level(manual_expander, BIT(2), 1);
                i2c_master_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_direction(GPIO_NUM_1, GPIO_MODE_INPUT);

            ESP_LOGI(TAG, "Manual TCA9554 init sequence completed (0x18).");
        } else {
            ESP_LOGW(TAG, "Manual TCA9554 init failed: %s", esp_err_to_name(err));
            manual_expander = NULL;
        }
    } else {
        ESP_LOGI(TAG, "Hardware v1.1 path chosen; skipping manual TCA9554 init.");
    }

    if (! boot_into_config_portal) {
        // 1) Explicitly initialize UART0 for PC GUI (USB-Serial)
        ESP_LOGI(TAG, "Initializing UART0 (GUI) explicitly");
        uart_config_t uart_config = {
            .baud_rate  = 115200,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t e = uart_param_config(GUI_UART_PORT, &uart_config);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "uart_param_config GUI failed: %s", esp_err_to_name(e));
        }
        e = uart_driver_install(GUI_UART_PORT, GUI_RX_BUF_SIZE * 2, 0, 0, NULL, 0);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install GUI failed: %s", esp_err_to_name(e));
        } else {
            ESP_LOGI(TAG, "UART driver installed on port %d (GUI)", (int)GUI_UART_PORT);
        }

        // 2) Initialize services that do not depend on esp_event loop
        location_service_init();
        log_heap_checkpoint("heap[startup: after location init]");
    } else {
        ESP_LOGI(TAG, "Skipping GUI UART and location service init in setup portal mode");
    }

    // 3) Initialize TCP/IP stack and event loop before UI
    esp_err_t e;
    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "esp_netif_init done (or already initialized)");
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "esp_event_loop_create_default done (or already created)");
    }
    log_heap_checkpoint("heap[startup: after netif/event loop]");
    // 4) UI / LVGL init
    auto board = create_board_for_hw(is_v1_1);
    if (! board) {
        ESP_LOGE(TAG, "Failed to allocate board object");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (! board->init()) {
        ESP_LOGE(TAG, "board->init() failed");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
#if LVGL_PORT_AVOID_TEARING_MODE
    if (! board->getLCD()->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM)) {
        ESP_LOGE(TAG, "configFrameBufferNumber(%d) failed", LVGL_PORT_DISP_BUFFER_NUM);
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    if (! board->begin()) {
        ESP_LOGE(TAG, "board->begin() failed");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Additional safeguard: if manual_expander exists, re-apply backlight level after Board begin */
    if (manual_expander) {
        ESP_LOGI(TAG, "Applying extra backlight level via manual_expander...");
        if (i2c_master_lock(1000)) {
            esp_io_expander_set_level(manual_expander, BIT(1) | BIT(7) | BIT(3), 1);
            i2c_master_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (! lvgl_port_init(board->getLCD(), board->getTouch())) {
        ESP_LOGE(TAG, "lvgl_port_init() failed");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    log_heap_checkpoint("heap[startup: after lvgl]");
    if (! boot_into_config_portal) {
        weather_integration_init();
        log_heap_checkpoint("heap[startup: after weather init]");
    } else {
        ESP_LOGI(TAG, "Skipping weather integration init in setup portal mode");
    }

    if (boot_into_config_portal) {
        esp_err_t portal_ret = config_portal_start();
        config_portal_show_screen(portal_ret == ESP_OK, portal_ret == ESP_OK ? NULL : esp_err_to_name(portal_ret));
        lvgl_port_start();
        ESP_LOGI(TAG, "Setup portal mode initialized");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ui_init();
    log_heap_checkpoint("heap[startup: after ui_init]");
    lv_timer_create(
        [](lv_timer_t* t) {
            (void)t;
            time_t now;
            struct tm ti;
            time(&now);
            localtime_r(&now, &ti);
            static int s_last_hour = -1;
            static int s_last_min  = -1;
            if (ti.tm_hour != s_last_hour || ti.tm_min != s_last_min) {
                s_last_hour = ti.tm_hour;
                s_last_min  = ti.tm_min;
                ui_update_time_display((uint8_t)ti.tm_hour, (uint8_t)ti.tm_min);
            }
        },
        1000, NULL);

    sleep_manager_set_manual_expander(manual_expander);
    sleep_manager_init(board);
    lvgl_port_start();
    sleep_manager_start();
    sleep_manager_exit_sleep();
    log_heap_checkpoint("heap[startup: after sleep]");
    ESP_LOGI(TAG, "UI initialized and displayed — now starting deferred services");

    // 5) Start deferred Wi-Fi init (so UI appears first)
    s_wifi_init_done = false;
    BaseType_t r = xTaskCreate(wifi_init_task, "wifi_init", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "wifi_init task create result: %d", (int)r);

    // 6) Start GUI UART RX task (handles JSON from PC)
    r = xTaskCreate(gui_uart_task, "gui_uart", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "gui_uart task create result: %d", (int)r);

    esp_err_t aux_udp_err = aux_udp_link_start();
    if (aux_udp_err != ESP_OK) {
        ESP_LOGE(TAG, "aux_udp_link_start failed: %s", esp_err_to_name(aux_udp_err));
    }

#if APP_HEAP_LOGGING_ENABLED
    // 7) Periodic PSRAM heap stats (helps diagnose fragmentation/leaks during UI use)
    r = xTaskCreate(psram_monitor_task, "psram_mon", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    ESP_LOGI(TAG, "psram_monitor task create result: %d", (int)r);
#endif
    log_heap_checkpoint("heap[startup: after deferred tasks]");

    ESP_LOGI(TAG, "End of app_main init (entering idle loop)");

    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
