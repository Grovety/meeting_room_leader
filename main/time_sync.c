// time_sync.c
#include "time_sync.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "lvgl.h"

/* External dependencies */
#include "location_service.h"
#include "wifi_manager.h" // uses wifi_manager_is_connected()

/* RTC interface */
#include "rtc_ds3231.h" /* added */

#ifdef __cplusplus
extern "C" {
#endif
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock(void);
#include "ui.h"
void ui_update_date_display(int year, int month, int day, int wday);
#ifdef __cplusplus
}
#endif

static const char* TAG = "time_sync";
static const char* NVS_NAMESPACE = "time_sync";
static const char* NVS_KEY_TZ_POSIX = "tz_posix";

static bool s_started = false;
static volatile bool s_synced  = false; // volatile because it is modified in a callback
static TaskHandle_t s_sntp_task_handle = NULL;
static bool s_sntp_started = false;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
static StaticTask_t *s_sntp_tcb = NULL;
static StackType_t *s_sntp_stack = NULL;
#endif

static char s_tz_iana[64] = {0};
static char s_tz_posix[64] = {0};
static char s_city[150]   = {0};
static char s_ntp_server0_name[INET6_ADDRSTRLEN] = {0};

typedef struct {
    uint8_t hour;
    uint8_t minute;
    int year, month, day, wday;
} ui_time_msg_t;

typedef struct {
    time_t epoch;
} time_sync_epoch_msg_t;

static void ui_apply_time_cb(void* p)
{
    ui_time_msg_t* m = (ui_time_msg_t*)p;
    if (!m) return;

    ui_update_time_display(m->hour, m->minute);
    ui_update_date_display(m->year, m->month, m->day, m->wday);

    lv_mem_free(m);
}

/* Helper: post time to UI (async) */
static void post_time_to_ui_from_epoch(time_t epoch)
{
    struct tm timeinfo;
    localtime_r(&epoch, &timeinfo);

    ui_time_msg_t* msg = (ui_time_msg_t*)lv_mem_alloc(sizeof(ui_time_msg_t));
    if (!msg) return;

    msg->hour   = (uint8_t)timeinfo.tm_hour;
    msg->minute = (uint8_t)timeinfo.tm_min;
    msg->year   = timeinfo.tm_year + 1900;
    msg->month  = timeinfo.tm_mon + 1;
    msg->day    = timeinfo.tm_mday;
    msg->wday   = timeinfo.tm_wday;

    if (!lvgl_port_lock(-1)) {
        lv_mem_free(msg);
        return;
    }

    lv_res_t rc = lv_async_call(ui_apply_time_cb, msg);
    lvgl_port_unlock();
    if (rc != LV_RES_OK) {
        lv_mem_free(msg);
    }
}

static void sntp_time_sync_dispatch_task(void* arg)
{
    time_sync_epoch_msg_t* msg = (time_sync_epoch_msg_t*)arg;
    if (!msg) {
        vTaskDelete(NULL);
        return;
    }

    post_time_to_ui_from_epoch(msg->epoch);

    if (rtc_ds3231_write_epoch(msg->epoch)) {
        ESP_LOGI(TAG, "SNTP -> RTC: wrote epoch %lld to RTC", (long long)msg->epoch);
    } else {
        ESP_LOGW(TAG, "SNTP -> RTC: failed to write epoch to RTC");
    }

    free(msg);
    vTaskDelete(NULL);
}

/* SNTP callback: called when SNTP updates system time */
static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    time_sync_epoch_msg_t* msg = NULL;
    BaseType_t rc = pdFAIL;

    if (!tv) return;

    s_synced = true;

    msg = (time_sync_epoch_msg_t*)malloc(sizeof(*msg));
    if (!msg) {
        ESP_LOGW(TAG, "SNTP callback: failed to allocate dispatch message");
        return;
    }
    msg->epoch = tv->tv_sec;

    rc = xTaskCreate(sntp_time_sync_dispatch_task, "time_sync_cb", 4096, msg, tskIDLE_PRIORITY + 2, NULL);
    if (rc != pdPASS) {
        ESP_LOGW(TAG, "SNTP callback: failed to create dispatch task (rc=%d)", (int)rc);
        free(msg);
    }
}

/* Try resolving the NTP host list; return true if at least one host resolves successfully.
   On successful resolve, fill resolved_ip (string "x.x.x.x" or IPv6). */
static bool resolve_ntp_hosts_once(char **hosts, size_t hosts_count,
                                   char *resolved_ip, size_t ip_len)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char addrstr[INET6_ADDRSTRLEN];

    for (size_t i = 0; i < hosts_count; ++i) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC; // request both v4 and v6
        hints.ai_socktype = SOCK_DGRAM;

        int rc = getaddrinfo(hosts[i], "123", &hints, &res);
        if (rc != 0 || res == NULL) {
            ESP_LOGW(TAG, "DNS: getaddrinfo('%s') failed, err=%d", hosts[i], rc);
            if (res) {
                freeaddrinfo(res);
                res = NULL;
            }
            continue;
        }

        // Prefer IPv4 first, otherwise use the first IPv6
        struct addrinfo *p = res;
        void *addr_ptr = NULL;
        int family = 0;
        bool found = false;

        // search for IPv4
        p = res;
        while (p) {
            if (p->ai_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in*)p->ai_addr;
                addr_ptr = &sin->sin_addr;
                family = AF_INET;
                found = true;
                break;
            }
            p = p->ai_next;
        }

        // if IPv4 is not found, use the first available address (possibly IPv6)
        if (!found) {
            p = res;
            while (p) {
                if (p->ai_family == AF_INET6) {
                    struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)p->ai_addr;
                    addr_ptr = &sin6->sin6_addr;
                    family = AF_INET6;
                    found = true;
                    break;
                }
                p = p->ai_next;
            }
        }

        if (found && addr_ptr) {
            inet_ntop(family, addr_ptr, addrstr, sizeof(addrstr));
            ESP_LOGI(TAG, "DNS: resolved %s -> %s", hosts[i], addrstr);

            if (resolved_ip && ip_len > 0) {
                strncpy(resolved_ip, addrstr, ip_len - 1);
                resolved_ip[ip_len - 1] = '\0';
            }

            freeaddrinfo(res);
            return true;
        }

        freeaddrinfo(res);
    }

    return false;
}

static bool load_persisted_timezone(char* posix_tz, size_t posix_tz_len)
{
    if (!posix_tz || posix_tz_len == 0) {
        return false;
    }

    posix_tz[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required = posix_tz_len;
    err = nvs_get_str(handle, NVS_KEY_TZ_POSIX, posix_tz, &required);
    nvs_close(handle);
    if (err != ESP_OK || posix_tz[0] == '\0') {
        posix_tz[0] = '\0';
        return false;
    }

    posix_tz[posix_tz_len - 1] = '\0';
    return true;
}

static void persist_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for timezone save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, NVS_KEY_TZ_POSIX, posix_tz);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Persisted timezone: %s", posix_tz);
    } else {
        ESP_LOGW(TAG, "Failed to persist timezone '%s': %s", posix_tz, esp_err_to_name(err));
    }
}

static bool resolve_posix_timezone(const char* tz_hint, const char* city, char* posix_tz, size_t posix_tz_len)
{
    if (!posix_tz || posix_tz_len == 0) {
        return false;
    }

    posix_tz[0] = '\0';

    if (tz_hint && tz_hint[0] != '\0') {
        if (strchr(tz_hint, '/') != NULL) {
            const char* converted = location_service_convert_iana_to_posix(tz_hint);
            if (converted && converted[0] != '\0' &&
                (strcmp(converted, "GMT0") != 0 || strcmp(tz_hint, "GMT0") == 0)) {
                strncpy(posix_tz, converted, posix_tz_len - 1);
                posix_tz[posix_tz_len - 1] = '\0';
                ESP_LOGI(TAG, "Timezone resolved from IANA: %s -> %s", tz_hint, posix_tz);
                return true;
            }
        } else {
            strncpy(posix_tz, tz_hint, posix_tz_len - 1);
            posix_tz[posix_tz_len - 1] = '\0';
            ESP_LOGI(TAG, "Timezone resolved from direct POSIX hint: %s", posix_tz);
            return true;
        }
    }

    if (city && city[0] != '\0') {
        const char* detected = location_service_detect_timezone_by_city(city);
        if (detected && detected[0] != '\0') {
            strncpy(posix_tz, detected, posix_tz_len - 1);
            posix_tz[posix_tz_len - 1] = '\0';
            ESP_LOGI(TAG, "Timezone resolved by city: %s -> %s", city, posix_tz);
            return true;
        }
    }

    strncpy(posix_tz, "GMT0", posix_tz_len - 1);
    posix_tz[posix_tz_len - 1] = '\0';
    ESP_LOGW(TAG, "Timezone fallback: GMT0");
    return false;
}

static void apply_posix_timezone(const char* posix_tz, const char* reason)
{
    const char* tz = (posix_tz && posix_tz[0] != '\0') ? posix_tz : "GMT0";

    setenv("TZ", tz, 1);
    tzset();
    strncpy(s_tz_posix, tz, sizeof(s_tz_posix) - 1);
    s_tz_posix[sizeof(s_tz_posix) - 1] = '\0';

    if (reason && reason[0] != '\0') {
        ESP_LOGI(TAG, "Timezone applied: %s (%s)", tz, reason);
    } else {
        ESP_LOGI(TAG, "Timezone applied: %s", tz);
    }

    if (lv_is_initialized()) {
        time_t now;
        time(&now);
        if (now > 1000000) {
            post_time_to_ui_from_epoch(now);
        }
    }
}

static void apply_timezone(const char* tz_hint, const char* city, bool persist)
{
    char posix_tz[64];
    resolve_posix_timezone(tz_hint, city, posix_tz, sizeof(posix_tz));
    apply_posix_timezone(posix_tz, persist ? "location update" : "runtime");
    if (persist) {
        persist_timezone(posix_tz);
    }
}

void time_sync_apply_saved_timezone(void)
{
    char posix_tz[64];
    if (load_persisted_timezone(posix_tz, sizeof(posix_tz))) {
        apply_posix_timezone(posix_tz, "persisted");
        return;
    }

    apply_posix_timezone("GMT0", "default");
}

static void sntp_task(void* arg);
static void time_sync_apply_runtime_context(void);
static void time_sync_start_sntp_client_once(void);

static void time_sync_apply_rtc_fallback(void)
{
    if (rtc_ds3231_init() == ESP_OK) {
        time_t rtc_epoch = 0;
        if (rtc_ds3231_read_epoch(&rtc_epoch)) {
            struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
            if (settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "System time set from RTC: %s", ctime(&rtc_epoch));
                post_time_to_ui_from_epoch(rtc_epoch);
            } else {
                ESP_LOGW(TAG, "settimeofday() failed when setting from RTC");
            }
        } else {
            ESP_LOGI(TAG, "RTC present but read_epoch failed or invalid time");
        }
    } else {
        ESP_LOGI(TAG, "No RTC detected or init failed — continuing with SNTP only");
    }

    ESP_LOGI(TAG, "RTC time can be shown immediately, but system sync is confirmed only after SNTP.");
}

static void time_sync_apply_runtime_context(void)
{
    apply_timezone(s_tz_iana, s_city, false);
    time_sync_apply_rtc_fallback();
}

static void time_sync_start_sntp_client_once(void)
{
    char *ntp_hosts[] = {
        "time.google.com",
        "pool.ntp.org",
        "time.cloudflare.com"
    };
    const size_t ntp_hosts_count = sizeof(ntp_hosts) / sizeof(ntp_hosts[0]);

    bool resolved_any = false;
    s_ntp_server0_name[0] = '\0';

    if (wifi_manager_is_connected()) {
        resolved_any = resolve_ntp_hosts_once(ntp_hosts, ntp_hosts_count,
                                             s_ntp_server0_name, sizeof(s_ntp_server0_name));
    } else {
        ESP_LOGW(TAG, "Wi-Fi is not connected yet; starting SNTP with hostnames");
    }

    esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    if (resolved_any && s_ntp_server0_name[0] != '\0') {
        ESP_LOGI(TAG, "DNS probe succeeded, but SNTP will use hostnames for resilience");
    } else {
        ESP_LOGW(TAG, "DNS probe did not resolve an NTP host yet; SNTP will still use hostnames");
    }

    esp_sntp_setservername(0, ntp_hosts[0]);
    esp_sntp_setservername(1, ntp_hosts[1]);
    esp_sntp_setservername(2, ntp_hosts[2]);

    sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP client initialized");
}

static bool create_sntp_task(void)
{
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    const size_t psram_stack_sizes[] = { 6 * 1024, 4 * 1024 };
    for (size_t i = 0; i < sizeof(psram_stack_sizes) / sizeof(psram_stack_sizes[0]); ++i) {
        size_t stack_bytes = psram_stack_sizes[i];
        StaticTask_t *tcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!tcb) {
            ESP_LOGW(TAG, "Failed to allocate SNTP TCB in internal RAM for stack=%u", (unsigned)stack_bytes);
            continue;
        }

        // SNTP task persists timezone to NVS, so its stack must stay in internal RAM.
        StackType_t *stack = (StackType_t*)heap_caps_malloc(stack_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!stack) {
            ESP_LOGW(TAG, "Failed to allocate SNTP stack in INTERNAL (%u bytes)", (unsigned)stack_bytes);
            heap_caps_free(tcb);
            continue;
        }

        TaskHandle_t handle = xTaskCreateStatic(
            sntp_task,
            "sntp_task",
            stack_bytes / sizeof(StackType_t),
            NULL,
            tskIDLE_PRIORITY + 5,
            stack,
            tcb);
        if (handle) {
            s_sntp_task_handle = handle;
            s_sntp_tcb = tcb;
            s_sntp_stack = stack;
            ESP_LOGI(TAG, "SNTP task created with INTERNAL stack=%u bytes", (unsigned)stack_bytes);
            return true;
        }

        ESP_LOGW(TAG, "xTaskCreateStatic failed for SNTP stack=%u", (unsigned)stack_bytes);
        heap_caps_free(stack);
        heap_caps_free(tcb);
    }
#endif

    const uint32_t dyn_stack_sizes[] = { 6 * 1024, 4 * 1024 };
    for (size_t i = 0; i < sizeof(dyn_stack_sizes) / sizeof(dyn_stack_sizes[0]); ++i) {
        uint32_t stack_bytes = dyn_stack_sizes[i];
        BaseType_t rc = xTaskCreate(sntp_task, "sntp_task", stack_bytes, NULL, tskIDLE_PRIORITY + 5, &s_sntp_task_handle);
        if (rc == pdPASS) {
            ESP_LOGI(TAG, "SNTP task created with dynamic stack=%u bytes", (unsigned)stack_bytes);
            return true;
        }
        ESP_LOGW(TAG, "xTaskCreate failed for SNTP stack=%u (rc=%d)", (unsigned)stack_bytes, (int)rc);
    }

    s_sntp_task_handle = NULL;
    ESP_LOGW(TAG, "Falling back to inline SNTP startup in caller task");
    time_sync_apply_runtime_context();
    time_sync_start_sntp_client_once();
    return true;
}

static void sntp_task(void* arg)
{
    (void)arg;
    s_sntp_task_handle = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG, "Starting SNTP task...");

    time_sync_apply_runtime_context();

    // 2) Start SNTP with infinite retries: temporary DNS/UDP failures no longer require reboot.
    const int wifi_wait_max = 60;
    const int max_wait_sync = 30;
    const int retry_delay_sec = 15;

    while (!s_synced) {
        int wifi_waited = 0;
        while (!wifi_manager_is_connected()) {
            if (wifi_waited < wifi_wait_max) {
                wifi_waited++;
                ESP_LOGI(TAG, "Waiting for Wi-Fi connection... (%d/%d)", wifi_waited, wifi_wait_max);
            } else if (((wifi_waited - wifi_wait_max) % 5) == 0) {
                ESP_LOGW(TAG, "Wi-Fi still not connected; SNTP retry postponed");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        time_sync_start_sntp_client_once();

        int waited = 0;
        while (!s_synced && waited++ < max_wait_sync && wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", waited, max_wait_sync);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (s_synced) {
            break;
        }

        esp_sntp_stop();

        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "Wi-Fi connection lost during SNTP sync. Will retry when network returns.");
            continue;
        }

        ESP_LOGW(TAG, "SNTP sync timeout after %d seconds. Retrying in %d seconds.", max_wait_sync, retry_delay_sec);
        for (int i = 0; i < retry_delay_sec && !s_synced; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "SNTP synchronized (confirmed by callback)");
    s_sntp_task_handle = NULL;
    vTaskDelete(NULL);
}

bool time_sync_start(const char* iana_tz, const char* city)
{
    if (iana_tz && iana_tz[0] != '\0') {
        strncpy(s_tz_iana, iana_tz, sizeof(s_tz_iana) - 1);
        s_tz_iana[sizeof(s_tz_iana) - 1] = '\0';
    } else {
        s_tz_iana[0] = '\0';
    }
    if (city && city[0] != '\0') {
        strncpy(s_city, city, sizeof(s_city) - 1);
        s_city[sizeof(s_city) - 1] = '\0';
    } else {
        s_city[0] = '\0';
    }

    apply_timezone(s_tz_iana, s_city, true);

    if (s_started) {
        ESP_LOGI(TAG, "%s already running; timezone/location updated",
                 s_sntp_task_handle ? "SNTP task" : "SNTP service");
        return true;
    }

    s_started = create_sntp_task();
    if (!s_started) {
        ESP_LOGE(TAG, "Failed to start SNTP task");
        return false;
    }

    return true;
}

bool time_sync_is_synced(void)
{
    if (s_synced) {
        return true;
    }

    if (s_sntp_started && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_synced = true;
        return true;
    }

    return s_synced;
}
