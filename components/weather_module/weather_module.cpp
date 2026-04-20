// weather_module.cpp - lower priority weather task, safe memory usage and mutex protection
#include "weather_module.h"
#include "weather_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"

static const char* TAG = "weather_module";

/* NVS keys & namespace */
static const char* NVS_NAMESPACE = "weather_store";
static const char* NVS_KEY_CURRENT = "current";
static const char* NVS_KEY_FORECAST = "forecast";
static const char* NVS_KEY_FCOUNT = "fcount";
static const char* NVS_KEY_TODAY = "today";

/* Module state */
static weather_config_t current_config;
static TaskHandle_t weather_task_handle = NULL;
static TaskHandle_t weather_cache_persist_task_handle = NULL;
static volatile bool module_running = false;
static SemaphoreHandle_t config_mutex = NULL;
static WeatherService weather_service;
static volatile bool force_update_requested = false;
static volatile bool weather_cache_persist_requested = false;

static weather_data_t cached_weather_data;
static weather_forecast_day_t cached_forecast[4];
static uint8_t cached_forecast_count = 0;
static weather_today_outlook_t cached_today_outlook;
static weather_data_status_t cached_status = WEATHER_DATA_STATUS_UNAVAILABLE;
static SemaphoreHandle_t data_mutex = NULL;

static void (*weather_update_callback)(const weather_data_t* data, const weather_forecast_day_t* forecast, uint8_t count) = NULL;

// At the top of the file, near other includes:
#include "lvgl.h" // required for lv_is_initialized()

static float weather_celsius_to_fahrenheit(float value)
{
    return (value * 9.0f / 5.0f) + 32.0f;
}

static float weather_fahrenheit_to_celsius(float value)
{
    return (value - 32.0f) * 5.0f / 9.0f;
}

static float weather_mps_to_mph(float value)
{
    return value * 2.2369363f;
}

static float weather_mph_to_mps(float value)
{
    return value / 2.2369363f;
}

static float weather_convert_temperature_value(float value, bool to_metric)
{
    return to_metric ? weather_fahrenheit_to_celsius(value) : weather_celsius_to_fahrenheit(value);
}

static float weather_convert_wind_speed_value(float value, bool to_metric)
{
    return to_metric ? weather_mph_to_mps(value) : weather_mps_to_mph(value);
}

static bool weather_api_key_looks_invalid(const char* api_key)
{
    if (!api_key || api_key[0] == '\0') {
        return false;
    }

    for (const unsigned char* p = (const unsigned char*)api_key; *p; ++p) {
        if (!isascii(*p) || !(isalnum(*p) || *p == '-' || *p == '_')) {
            return true;
        }
    }

    return false;
}

static bool weather_copy_cached_snapshot(weather_data_t* data, weather_forecast_day_t* forecast, uint8_t* count)
{
    bool has_data = false;

    if (!data || !forecast || !count) {
        return false;
    }

    memset(data, 0, sizeof(*data));
    memset(forecast, 0, sizeof(weather_forecast_day_t) * 4);
    *count = 0;

    if (data_mutex) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
    }

    memcpy(data, &cached_weather_data, sizeof(*data));
    if (cached_forecast_count > 0) {
        uint8_t safe_count = (cached_forecast_count > 4) ? 4 : cached_forecast_count;
        memcpy(forecast, cached_forecast, sizeof(weather_forecast_day_t) * safe_count);
        *count = safe_count;
    }
    has_data = (cached_weather_data.timestamp > 0);

    if (data_mutex) {
        xSemaphoreGive(data_mutex);
    }

    return has_data;
}

static void weather_emit_cached_update_snapshot(void)
{
    weather_data_t data = {};
    weather_forecast_day_t forecast[4] = {};
    uint8_t count = 0;

    if (!weather_update_callback) {
        return;
    }

    if (!weather_copy_cached_snapshot(&data, forecast, &count)) {
        return;
    }

    weather_update_callback(&data, forecast, count);
}

// ----------------- insert/replace this implementation -----------------
static void weather_deferred_callback_task(void* arg) {
    (void)arg;
    const int max_wait_ms = 5000; // wait up to 5 seconds, then return anyway (prevents infinite waiting)
    const int step_ms = 100;
    int waited = 0;

    // Wait and check lv_is_initialized()
    while (!lv_is_initialized() && waited < max_wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }

    weather_emit_cached_update_snapshot();

    vTaskDelete(NULL);
}

void weather_module_set_update_callback(void (*callback)(const weather_data_t*, const weather_forecast_day_t*, uint8_t)) {
    weather_update_callback = callback;
    ESP_LOGI(TAG, "Weather update callback set");

    // If cached data exists, return it safely:
    if (weather_update_callback && cached_weather_data.timestamp > 0) {
        // If LVGL is already ready, return immediately (fast path)
        if (lv_is_initialized()) {
            weather_emit_cached_update_snapshot();
        } else {
            // LVGL is not ready yet; create a short delayed task that waits and returns data.
            // This is a one-shot task and finishes quickly.
            BaseType_t rc = xTaskCreate(
                weather_deferred_callback_task,
                "weather_cb_def",
                4096,
                NULL,
                tskIDLE_PRIORITY + 1,
                NULL
            );
            if (rc != pdPASS) {
                ESP_LOGW(TAG, "Failed to create deferred weather callback task (rc=%d). Will not call callback now.", (int)rc);
            } else {
                ESP_LOGI(TAG, "Deferred weather callback task created to deliver cached data after LVGL init");
            }
        }
    }
}

static uint32_t get_current_time(void) {
    time_t now;
    time(&now);
    return (uint32_t)now;
}

/* ---------------- NVS cache helpers ---------------- */

/*
 * Notes:
 * - save_cache_to_nvs_locked() assumes caller already holds data_mutex.
 * - save_cache_to_nvs() wraps the call with mutex.
 * - load_cache_from_nvs() can be called without mutex (in init), but after loading
 *   cached_* are used under data_mutex protection in the rest of the code.
 */

static bool save_cache_to_nvs_locked(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for save failed: %s", esp_err_to_name(e));
        return false;
    }

    esp_err_t r = nvs_set_blob(h, NVS_KEY_CURRENT, &cached_weather_data, sizeof(cached_weather_data));
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "NVS set_blob(current) failed: %s", esp_err_to_name(r));
        nvs_close(h);
        return false;
    }

    if (cached_forecast_count > 0) {
        uint8_t cnt = cached_forecast_count;
        r = nvs_set_u8(h, NVS_KEY_FCOUNT, cnt);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "NVS set_u8(fcount) failed: %s", esp_err_to_name(r));
            nvs_close(h);
            return false;
        }
        size_t fsize = sizeof(weather_forecast_day_t) * cnt;
        r = nvs_set_blob(h, NVS_KEY_FORECAST, &cached_forecast, fsize);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "NVS set_blob(forecast) failed: %s", esp_err_to_name(r));
            nvs_close(h);
            return false;
        }
    } else {
        // remove old keys if present
        nvs_erase_key(h, NVS_KEY_FCOUNT);
        nvs_erase_key(h, NVS_KEY_FORECAST);
    }

    r = nvs_set_blob(h, NVS_KEY_TODAY, &cached_today_outlook, sizeof(cached_today_outlook));
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "NVS set_blob(today) failed: %s", esp_err_to_name(r));
        nvs_close(h);
        return false;
    }

    r = nvs_commit(h);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(r));
        nvs_close(h);
        return false;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Saved weather cache to NVS (time=%u, forecast_count=%u)", (unsigned)cached_weather_data.timestamp, (unsigned)cached_forecast_count);
    return true;
}

static bool save_cache_to_nvs(void) {
    if (!data_mutex) return false;
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    bool r = save_cache_to_nvs_locked();
    xSemaphoreGive(data_mutex);
    return r;
}

static void weather_cache_persist_task(void* arg) {
    (void)arg;

    for (;;) {
        weather_cache_persist_requested = false;
        save_cache_to_nvs();
        if (!weather_cache_persist_requested) {
            break;
        }
    }

    weather_cache_persist_task_handle = NULL;
    vTaskDelete(NULL);
}

static void schedule_cache_persist(void) {
    weather_cache_persist_requested = true;
    if (weather_cache_persist_task_handle) {
        return;
    }

    TaskHandle_t handle = NULL;
    BaseType_t rc =
        xTaskCreatePinnedToCore(weather_cache_persist_task, "weather_nvs", 4096, NULL, tskIDLE_PRIORITY + 1, &handle, 1);
    if (rc != pdPASS) {
        rc = xTaskCreate(weather_cache_persist_task, "weather_nvs", 4096, NULL, tskIDLE_PRIORITY + 1, &handle);
    }

    if (rc == pdPASS) {
        weather_cache_persist_task_handle = handle;
    } else {
        ESP_LOGW(TAG, "Failed to create weather_nvs task (res=%d)", (int)rc);
    }
}

static bool load_cache_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "No weather cache in NVS (nvs_open -> %s)", esp_err_to_name(e));
        return false;
    }

    // current
    size_t need = sizeof(cached_weather_data);
    esp_err_t r = nvs_get_blob(h, NVS_KEY_CURRENT, &cached_weather_data, &need);
    if (r != ESP_OK || need != sizeof(cached_weather_data)) {
        // missing/corrupted - clear it
        memset(&cached_weather_data, 0, sizeof(cached_weather_data));
        cached_forecast_count = 0;
        nvs_close(h);
        ESP_LOGI(TAG, "No current weather blob (nvs_get_blob -> %s)", esp_err_to_name(r));
        return false;
    }

    // forecast count
    uint8_t fcount = 0;
    if (nvs_get_u8(h, NVS_KEY_FCOUNT, &fcount) == ESP_OK && fcount > 0 && fcount <= 4) {
        size_t fsize = sizeof(weather_forecast_day_t) * fcount;
        size_t got = fsize;
        esp_err_t rr = nvs_get_blob(h, NVS_KEY_FORECAST, &cached_forecast, &got);
        if (rr == ESP_OK && got == fsize) {
            cached_forecast_count = fcount;
        } else {
            cached_forecast_count = 0;
            memset(&cached_forecast, 0, sizeof(cached_forecast));
        }
    } else {
        cached_forecast_count = 0;
    }

    need = sizeof(cached_today_outlook);
    r = nvs_get_blob(h, NVS_KEY_TODAY, &cached_today_outlook, &need);
    if (r != ESP_OK || need != sizeof(cached_today_outlook)) {
        memset(&cached_today_outlook, 0, sizeof(cached_today_outlook));
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded weather cache: timestamp=%u, forecast_count=%u", (unsigned)cached_weather_data.timestamp, (unsigned)cached_forecast_count);
    return (cached_weather_data.timestamp > 0);
}

/* --- weather task --- */
static void weather_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Weather task started");
    vTaskDelay(pdMS_TO_TICKS(4000));

    // Initialize service under mutex protection (copy config)
    weather_config_t local_cfg;
    memset(&local_cfg, 0, sizeof(local_cfg));
    if (config_mutex) {
        xSemaphoreTake(config_mutex, portMAX_DELAY);
        memcpy(&local_cfg, &current_config, sizeof(weather_config_t));
        xSemaphoreGive(config_mutex);
    }

    if (!weather_service.init(&local_cfg)) {
        ESP_LOGE(TAG, "Weather task: Failed to init weather service");
        module_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Using location: %s (%.6f, %.6f)",
             local_cfg.city, local_cfg.latitude, local_cfg.longitude);

    const TickType_t update_interval_ticks = pdMS_TO_TICKS((uint64_t)local_cfg.update_interval_sec * 1000ULL);
    TickType_t last_update = 0;

    uint32_t forecast_counter = 0;

    while (module_running) {
        TickType_t now = xTaskGetTickCount();

        if ((now - last_update) >= update_interval_ticks || last_update == 0 || force_update_requested) {
            ESP_LOGI(TAG, "Updating weather...");
            force_update_requested = false;

            weather_data_t data;
            memset(&data, 0, sizeof(data));
            if (config_mutex) {
                xSemaphoreTake(config_mutex, portMAX_DELAY);
            }
            const bool current_ok = weather_service.fetch_current_weather(&data);
            if (config_mutex) {
                xSemaphoreGive(config_mutex);
            }
            if (current_ok) {
                if (data_mutex) {
                    xSemaphoreTake(data_mutex, portMAX_DELAY);
                    cached_status = WEATHER_DATA_STATUS_OK;
                    xSemaphoreGive(data_mutex);
                } else {
                    cached_status = WEATHER_DATA_STATUS_OK;
                }
                if (data.timestamp == 0) data.timestamp = get_current_time();

                if (data.pop < 0) data.pop = 0;
                if (data.rain < 0) data.rain = 0;
                if (data.snow < 0) data.snow = 0;

                if (data_mutex) {
                    xSemaphoreTake(data_mutex, portMAX_DELAY);
                    memcpy(&cached_weather_data, &data, sizeof(weather_data_t));
                    xSemaphoreGive(data_mutex);
                } else {
                    memcpy(&cached_weather_data, &data, sizeof(weather_data_t));
                }
                schedule_cache_persist();

                if (forecast_counter == 0) {
                    weather_forecast_day_t forecast_local[4];
                    weather_today_outlook_t today_outlook_local;
                    bool forecast_updated = false;
                    memset(forecast_local, 0, sizeof(forecast_local));
                    memset(&today_outlook_local, 0, sizeof(today_outlook_local));
                    uint8_t count = 0;
                    if (config_mutex) {
                        xSemaphoreTake(config_mutex, portMAX_DELAY);
                    }
                    const bool forecast_ok = weather_service.fetch_forecast(forecast_local, &count, &today_outlook_local);
                    if (config_mutex) {
                        xSemaphoreGive(config_mutex);
                    }
                    if (forecast_ok) {
                        if (data_mutex) {
                            xSemaphoreTake(data_mutex, portMAX_DELAY);
                            memset(cached_forecast, 0, sizeof(cached_forecast));
                            if (count > 4) count = 4;
                            memcpy(cached_forecast, forecast_local, sizeof(weather_forecast_day_t) * count);
                            cached_forecast_count = count;
                            memcpy(&cached_today_outlook, &today_outlook_local, sizeof(cached_today_outlook));
                            xSemaphoreGive(data_mutex);
                        } else {
                            memset(cached_forecast, 0, sizeof(cached_forecast));
                            if (count > 4) count = 4;
                            memcpy(cached_forecast, forecast_local, sizeof(weather_forecast_day_t) * count);
                            cached_forecast_count = count;
                            memcpy(&cached_today_outlook, &today_outlook_local, sizeof(cached_today_outlook));
                        }
                        schedule_cache_persist();
                        forecast_updated = true;
                        ESP_LOGI(TAG, "Forecast updated: %d days", count);
                    } else {
                        if (data_mutex) {
                            xSemaphoreTake(data_mutex, portMAX_DELAY);
                            memset(&cached_today_outlook, 0, sizeof(cached_today_outlook));
                            xSemaphoreGive(data_mutex);
                        } else {
                            memset(&cached_today_outlook, 0, sizeof(cached_today_outlook));
                        }
                        ESP_LOGW(TAG, "Failed to fetch forecast");
                    }
                    forecast_counter = forecast_updated ? 5 : 0;
                } else {
                    --forecast_counter;
                }

                if (weather_update_callback) {
                    weather_emit_cached_update_snapshot();
                }

                last_update = now;
                ESP_LOGI(TAG, "Weather updated: %.1f°C, %s", data.temperature, data.description);
            } else {
                weather_data_status_t new_status = WEATHER_DATA_STATUS_UNAVAILABLE;
                const int last_http_status = weather_service_get_last_http_status();
                if (config_mutex) {
                    xSemaphoreTake(config_mutex, portMAX_DELAY);
                    if (current_config.api_key[0] == '\0') {
                        new_status = WEATHER_DATA_STATUS_API_KEY_REQUIRED;
                    } else if (weather_api_key_looks_invalid(current_config.api_key)) {
                        new_status = WEATHER_DATA_STATUS_INVALID_API_KEY;
                    } else if (last_http_status == 401) {
                        new_status = WEATHER_DATA_STATUS_INVALID_API_KEY;
                    }
                    xSemaphoreGive(config_mutex);
                }
                if (data_mutex) {
                    xSemaphoreTake(data_mutex, portMAX_DELAY);
                    cached_status = new_status;
                    xSemaphoreGive(data_mutex);
                } else {
                    cached_status = new_status;
                }
                if (weather_update_callback) {
                    weather_emit_cached_update_snapshot();
                }
                ESP_LOGE(TAG, "Failed to fetch current weather");
                vTaskDelay(pdMS_TO_TICKS(30000));
            }
        }

        // Very frequent short sleep to give other tasks CPU time
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Weather task stopped");
    weather_task_handle = NULL;
    vTaskDelete(NULL);
}

/* --- module lifecycle --- */

void weather_module_init(void) {
    if (!config_mutex) config_mutex = xSemaphoreCreateMutex();
    if (!data_mutex) data_mutex = xSemaphoreCreateMutex();

    if (!config_mutex || !data_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return;
    }

    memset(&current_config, 0, sizeof(current_config));
    memset(&cached_weather_data, 0, sizeof(cached_weather_data));
    memset(cached_forecast, 0, sizeof(cached_forecast));
    cached_forecast_count = 0;
    memset(&cached_today_outlook, 0, sizeof(cached_today_outlook));
    cached_status = WEATHER_DATA_STATUS_UNAVAILABLE;
    module_running = false;
    weather_task_handle = NULL;
    weather_update_callback = NULL;
    weather_cache_persist_task_handle = NULL;
    weather_cache_persist_requested = false;

    // Load persisted cache from NVS (if any)
    load_cache_from_nvs();

    ESP_LOGI(TAG, "Weather module initialized");
}

void weather_module_set_config(const weather_config_t* config) {
    if (!config_mutex || !config) return;

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    memcpy(&current_config, config, sizeof(weather_config_t));
    if (module_running) {
        if (!weather_service.init(&current_config)) {
            ESP_LOGW(TAG, "Failed to apply updated weather config to running service");
        }
    }
    xSemaphoreGive(config_mutex);

    ESP_LOGI(TAG, "Weather config updated");
}

void weather_module_request_update(void) {
    if (!module_running || !weather_task_handle) {
        return;
    }

    force_update_requested = true;
}

void weather_module_convert_cached_units(bool to_metric)
{
    if (data_mutex) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
    }

    if (cached_weather_data.timestamp > 0) {
        cached_weather_data.temperature = weather_convert_temperature_value(cached_weather_data.temperature, to_metric);
        cached_weather_data.feels_like = weather_convert_temperature_value(cached_weather_data.feels_like, to_metric);
        cached_weather_data.wind_speed = weather_convert_wind_speed_value(cached_weather_data.wind_speed, to_metric);
    }

    for (uint8_t i = 0; i < cached_forecast_count && i < 4; ++i) {
        cached_forecast[i].temp_day = weather_convert_temperature_value(cached_forecast[i].temp_day, to_metric);
        cached_forecast[i].temp_night = weather_convert_temperature_value(cached_forecast[i].temp_night, to_metric);
        cached_forecast[i].feels_like_day = weather_convert_temperature_value(cached_forecast[i].feels_like_day, to_metric);
        cached_forecast[i].wind_speed = weather_convert_wind_speed_value(cached_forecast[i].wind_speed, to_metric);
    }

    if (data_mutex) {
        xSemaphoreGive(data_mutex);
    }

    schedule_cache_persist();

    if (weather_update_callback) {
        weather_emit_cached_update_snapshot();
    }
}

#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
static StaticTask_t *weather_tcb = NULL;
static StackType_t  *weather_stack = NULL;
#endif

void weather_module_start(void) {
    if (module_running) {
        ESP_LOGW(TAG, "Module already running");
        return;
    }

    BaseType_t res = pdFAIL;
    TaskHandle_t handle = NULL;

#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    const int static_stack_sizes[] = { 16 * 1024, 12 * 1024, 8 * 1024 }; // bytes
    for (size_t i = 0; i < sizeof(static_stack_sizes)/sizeof(static_stack_sizes[0]); ++i) {
        int sz_bytes = static_stack_sizes[i];

        StaticTask_t *tcb = (StaticTask_t*) heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!tcb) {
            ESP_LOGW(TAG, "weather_module: Failed to alloc TCB in INTERNAL for attempt %u", (unsigned)(i+1));
            continue;
        }

        StackType_t *stack = (StackType_t*) heap_caps_malloc(sz_bytes, MALLOC_CAP_SPIRAM);
        if (!stack) {
            ESP_LOGW(TAG, "weather_module: Failed to alloc stack in SPIRAM (%d bytes) for attempt %u", sz_bytes, (unsigned)(i+1));
            heap_caps_free(tcb);
            continue;
        }

        UBaseType_t stack_depth = (UBaseType_t)(sz_bytes / sizeof(StackType_t));
        TaskHandle_t h = xTaskCreateStatic(weather_task, "weather_task", stack_depth, NULL, 1, stack, tcb);
        if (h) {
            ESP_LOGI(TAG, "weather_module: weather_task static created with stack=%d (SPIRAM) and TCB in INTERNAL", sz_bytes);
            weather_tcb = tcb;
            weather_stack = stack;
            handle = h;
            res = pdPASS;
            break;
        }

        ESP_LOGW(TAG, "weather_module: xTaskCreateStatic failed for stack=%d. Freeing allocations and trying next.", sz_bytes);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif

    if (res != pdPASS) {
        // Create task with LOW priority so LVGL/UI always have priority
        const int dyn_stack_bytes = 8192;
        BaseType_t rc = xTaskCreatePinnedToCore(weather_task, "weather_task", dyn_stack_bytes, NULL, tskIDLE_PRIORITY + 1, &handle, 1);
        if (rc == pdPASS) {
            ESP_LOGI(TAG, "weather_module: weather_task created pinned to core 1 with low priority (stack=%d)", dyn_stack_bytes);
            res = pdPASS;
        } else {
            ESP_LOGW(TAG, "xTaskCreatePinnedToCore failed (res=%d). Trying xTaskCreate...", (int)rc);
            rc = xTaskCreate(weather_task, "weather_task", dyn_stack_bytes, NULL, tskIDLE_PRIORITY + 1, &handle);
            if (rc == pdPASS) {
                ESP_LOGI(TAG, "weather_module: weather_task created with low priority (stack=%d)", dyn_stack_bytes);
                res = pdPASS;
            } else {
                ESP_LOGW(TAG, "xTaskCreate failed for stack=%d (res=%d). FREE_HEAP=%u", dyn_stack_bytes, (int)rc, (unsigned)esp_get_free_heap_size());
            }
        }
    }

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create weather_task with any tested method/stack size. Module will not run. FREE_HEAP=%u", (unsigned)esp_get_free_heap_size());
        module_running = false;
        weather_task_handle = NULL;
        return;
    }

    // success
    module_running = true;
    weather_task_handle = handle;
    ESP_LOGI(TAG, "Weather module started (task handle=%p)", (void*)weather_task_handle);
}

void weather_module_stop(void) {
    if (!module_running) return;

    module_running = false;

    if (weather_task_handle) {
        // give the task a bit of time to shut down cleanly
        vTaskDelay(pdMS_TO_TICKS(100));
        // if task did not stop, delete it
        vTaskDelete(weather_task_handle);
        weather_task_handle = NULL;
    }

    if (weather_cache_persist_task_handle) {
        vTaskDelete(weather_cache_persist_task_handle);
        weather_cache_persist_task_handle = NULL;
    }
    weather_cache_persist_requested = false;

#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    if (weather_tcb) {
        heap_caps_free(weather_tcb);
        weather_tcb = NULL;
    }
    if (weather_stack) {
        heap_caps_free(weather_stack);
        weather_stack = NULL;
    }
#endif

    ESP_LOGI(TAG, "Weather module stopped");
}

bool weather_module_is_running(void) {
    return module_running;
}

bool weather_get_current_data(weather_data_t* data) {
    bool has_data = false;

    if (!data || !data_mutex) return false;

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    memcpy(data, &cached_weather_data, sizeof(weather_data_t));
    has_data = (cached_weather_data.timestamp > 0);
    xSemaphoreGive(data_mutex);

    return has_data;
}

bool weather_get_forecast_data(weather_forecast_day_t* forecast, uint8_t* count) {
    bool has_forecast = false;

    if (!forecast || !count || !data_mutex) return false;

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (cached_forecast_count > 0) {
        memcpy(forecast, cached_forecast, sizeof(weather_forecast_day_t) * cached_forecast_count);
    }
    *count = cached_forecast_count;
    has_forecast = (cached_forecast_count > 0);
    xSemaphoreGive(data_mutex);

    return has_forecast;
}

bool weather_get_today_outlook(weather_today_outlook_t* outlook) {
    if (!outlook) {
        return false;
    }

    memset(outlook, 0, sizeof(*outlook));
    if (data_mutex) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        memcpy(outlook, &cached_today_outlook, sizeof(*outlook));
        xSemaphoreGive(data_mutex);
    } else {
        memcpy(outlook, &cached_today_outlook, sizeof(*outlook));
    }

    return outlook->valid;
}

weather_data_status_t weather_module_get_status(void) {
    weather_data_status_t status = WEATHER_DATA_STATUS_UNAVAILABLE;

    if (config_mutex) {
        xSemaphoreTake(config_mutex, portMAX_DELAY);
        if (current_config.api_key[0] == '\0') {
            xSemaphoreGive(config_mutex);
            return WEATHER_DATA_STATUS_API_KEY_REQUIRED;
        }
        if (weather_api_key_looks_invalid(current_config.api_key)) {
            xSemaphoreGive(config_mutex);
            return WEATHER_DATA_STATUS_INVALID_API_KEY;
        }
        xSemaphoreGive(config_mutex);
    }

    if (data_mutex) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        status = cached_status;
        xSemaphoreGive(data_mutex);
    } else {
        status = cached_status;
    }

    return status;
}
