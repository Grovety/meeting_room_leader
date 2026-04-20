// wifi_manager.c
#include "wifi_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

/* For a portable critical-section API (Xtensa requires portMUX_TYPE). */
#include "freertos/portmacro.h"

static const char* TAG          = "wifi_manager";
static bool s_wifi_connected    = false;
static bool s_is_initialized    = false;
static bool s_manual_disconnect = false;
static bool s_auto_reconnect_enabled = true;

/* Mutex to prevent concurrent config/apply attempts from different tasks */
static SemaphoreHandle_t s_wifi_mutex = NULL;

/* Spinlock / mux used to protect creation of the semaphore in a safe way */
static portMUX_TYPE s_mutex_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Forward declarations */
static void ensure_mutex_created(void);
static void configure_wifi_runtime_locked(void);
static bool wifi_manager_lock_ticks(TickType_t timeout_ticks);

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_manual_disconnect && s_auto_reconnect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_manual_disconnect) {
            ESP_LOGI(TAG, "WiFi disconnected by user request");
        } else if (!s_auto_reconnect_enabled) {
            ESP_LOGI(TAG, "WiFi disconnected while auto reconnect is suppressed");
        } else {
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected    = true;
        s_manual_disconnect = false;
        ESP_LOGI(TAG, "WIFI:OK");
    }
}

bool wifi_manager_is_connected(void) { return s_wifi_connected; }

void wifi_manager_wait_connected(void)
{
    while (! s_wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t wifi_manager_disconnect(bool forget_credentials)
{
    if (!wifi_manager_lock_ticks(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "Timeout obtaining wifi_manager mutex for disconnect");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret       = ESP_OK;
    s_manual_disconnect = true;
    s_wifi_connected    = false;

    if (forget_credentials) {
        nvs_handle_t handle;
        esp_err_t nvs_err = nvs_open("storage", NVS_READWRITE, &handle);
        if (nvs_err == ESP_OK) {
            nvs_set_str(handle, "wifi_ssid", "");
            nvs_set_str(handle, "wifi_pass", "");
            nvs_err = nvs_commit(handle);
            nvs_close(handle);
        }

        if (nvs_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to clear saved WiFi credentials: %s", esp_err_to_name(nvs_err));
            ret = nvs_err;
        }

        if (s_is_initialized) {
            wifi_config_t empty_cfg = {0};
            esp_err_t cfg_err       = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
            if (cfg_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to clear runtime WiFi config: %s", esp_err_to_name(cfg_err));
                if (ret == ESP_OK) {
                    ret = cfg_err;
                }
            }
        }
    }

    if (s_is_initialized) {
        esp_err_t disc_err = esp_wifi_disconnect();
        if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_INIT && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "esp_wifi_disconnect returned %s", esp_err_to_name(disc_err));
            if (ret == ESP_OK) {
                ret = disc_err;
            }
        }
    }

    wifi_manager_unlock();
    return ret;
}

esp_err_t wifi_manager_save_credentials(const char* ssid, const char* pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    nvs_set_str(handle, "wifi_ssid", ssid);
    nvs_set_str(handle, "wifi_pass", pass);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_fetch_credentials(char* ssid, char* pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;
    size_t size_s = 33, size_p = 65;
    err = nvs_get_str(handle, "wifi_ssid", ssid, &size_s);
    if (err == ESP_OK)
        err = nvs_get_str(handle, "wifi_pass", pass, &size_p);
    nvs_close(handle);
    return err;
}

static void ensure_mutex_created(void)
{
    /* Use portENTER_CRITICAL/portEXIT_CRITICAL with an explicit spinlock,
       because these macros require an argument on this platform */
    portENTER_CRITICAL(&s_mutex_spinlock);
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_wifi_mutex == NULL) {
            ESP_LOGW(TAG, "Failed to create wifi mutex");
        } else {
            ESP_LOGD(TAG, "wifi mutex created");
        }
    }
    portEXIT_CRITICAL(&s_mutex_spinlock);
}

static bool wifi_manager_lock_ticks(TickType_t timeout_ticks)
{
    ensure_mutex_created();
    if (!s_wifi_mutex) {
        return false;
    }

    return xSemaphoreTakeRecursive(s_wifi_mutex, timeout_ticks) == pdTRUE;
}

bool wifi_manager_lock(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return wifi_manager_lock_ticks(timeout_ticks);
}

void wifi_manager_set_auto_reconnect_enabled(bool enabled) { s_auto_reconnect_enabled = enabled; }

bool wifi_manager_is_auto_reconnect_enabled(void) { return s_auto_reconnect_enabled; }

void wifi_manager_unlock(void)
{
    if (s_wifi_mutex) {
        xSemaphoreGiveRecursive(s_wifi_mutex);
    }
}

static void configure_wifi_runtime_locked(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) returned %s", esp_err_to_name(err));
    }
}

/*
 * wifi_manager_init_sta
 *  - Initializes the Wi-Fi stack on first call.
 *  - If ssid != NULL && strlen(ssid) > 0, tries to apply config and connect.
 *
 * Returns esp_err_t (ESP_OK on normal flow). Does not call ESP_ERROR_CHECK for operations
 * that may return ESP_ERR_WIFI_STATE; those cases are handled locally.
 */
esp_err_t wifi_manager_init_sta(const char* ssid, const char* pass)
{
    if (!wifi_manager_lock_ticks(pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "Timeout obtaining wifi_manager mutex");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    if (! s_is_initialized) {
        ESP_LOGI(TAG, "Initializing WiFi stack...");
        esp_err_t err = esp_netif_init();
        /* tolerate esp_netif already initialized (ESP_ERR_INVALID_STATE) */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err                    = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_event_handler_register (WIFI_EVENT) failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_event_handler_register (IP_EVENT) failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }
        configure_wifi_runtime_locked();
        s_is_initialized = true;
    } else {
        configure_wifi_runtime_locked();
    }

    if (ssid != NULL && strlen(ssid) > 0) {
        s_manual_disconnect       = false;
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

        // try a graceful disconnect, but do not rely on immediate state transition
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "esp_wifi_disconnect returned %s", esp_err_to_name(err));
            // not fatal, continue trying to apply configuration
        }

        ESP_LOGI(TAG, "Applying WiFi config for SSID: %s", ssid);

        const int max_retries = 20;
        int retries           = 0;
        err                   = ESP_ERR_WIFI_STATE;
        while (retries++ < max_retries) {
            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if (err == ESP_OK)
                break;
            if (err == ESP_ERR_WIFI_STATE) {
                // module is connecting; wait and retry
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            // other error: log and exit
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
            ret = err;
            goto out;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_config still failed after %d retries (%s). Restarting WiFi and retrying...",
                     max_retries, esp_err_to_name(err));
            // Try a soft Wi-Fi stack restart and make one more attempt
            esp_err_t s_err = esp_wifi_stop();
            if (s_err != ESP_OK && s_err != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(TAG, "esp_wifi_stop returned %s", esp_err_to_name(s_err));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            s_err = esp_wifi_start();
            if (s_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_start (after stop) failed: %s", esp_err_to_name(s_err));
                ret = s_err;
                goto out;
            }
            configure_wifi_runtime_locked();

            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_set_config failed after restart: %s", esp_err_to_name(err));
                ret = err;
                goto out;
            }
        }

        // Config applied successfully; trying to connect
        esp_err_t conn_err = esp_wifi_connect();
        if (conn_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(conn_err));
            // non-fatal situation; return code for further handling
            ret = conn_err;
            goto out;
        }
    }

out:
    wifi_manager_unlock();
    return ret;
}
