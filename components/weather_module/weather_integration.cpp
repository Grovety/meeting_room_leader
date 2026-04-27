// weather_integration.cpp  (fixed: non-blocking start + WIFI/IP event handler)
// Reworked to remove reference to weather_module_request_update (which may not exist)
// and to register a proper IP event handler function.

#include "weather_integration.h"
#include "weather_module.h"
#include "weather_ui.h"
#include "weather_widget.h"
#include "language_manager.h"

#include <esp_log.h>
#include <string.h>
#include <stdio.h>
#include <cctype>
#include <cstring>
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "location_service.h"
#include "time_sync.h"
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock(void);
#ifdef __cplusplus
}
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_event.h>
#include <esp_netif.h>

static const char* TAG = "weather_integration";

static bool integration_initialized = false;
static weather_config_t current_config;
static weather_widget_t* weather_widget = NULL;
static const char* kWeatherConfigNamespace = "weather_cfg";
static const char* kWeatherApiKeyKey = "api_key";
static const char* kWeatherUnitsKey = "metric";

/* Background task control */
static volatile bool s_start_task_running = false;
static volatile bool s_start_task_cancel = false;
static TaskHandle_t s_ip_start_deferred_task = NULL;
static constexpr uint32_t kWeatherIpStartTaskStackBytes = 4 * 1024;

/* WiFi/IP event handler instance handle */
static esp_event_handler_instance_t s_ip_event_handle = NULL;

/* Forward declarations */
void weather_integration_show_fullscreen(void);

// --- Async UI update: copy data and schedule lv_async_call ---
#include "lvgl.h" // ensure this include exists at the top of the file

struct AsyncWeatherUpdate {
    weather_data_t data;
    weather_forecast_day_t forecast[4];
    uint8_t count;
};

static void weather_update_async_cb(void* user_data) {
    AsyncWeatherUpdate* upd = static_cast<AsyncWeatherUpdate*>(user_data);
    if (!upd) return;

    // Update widget (widget_update may use LVGL API)
    if (weather_widget) {
        weather_widget_update(weather_widget, &upd->data);
        // Do not log large payloads here
    }

    // Update fullscreen UI (safe in LVGL context)
    WeatherUI* ui = WeatherUI::get_instance();
    if (ui && ui->is_initialized()) {
        ui->update_current_weather(&upd->data);
        if (upd->count > 0) {
            ui->update_forecast(upd->forecast, upd->count);
        }
    }

    // Free memory allocated in the worker thread
    heap_caps_free(upd);
}

static void weather_data_update_callback(const weather_data_t* data,
                                         const weather_forecast_day_t* forecast,
                                         uint8_t count) {
    // Briefly: log arrival in worker thread, but DO NOT call LVGL here.
    ESP_LOGI(TAG, "Weather data updated callback (received)");

    if (!data) return;

    AsyncWeatherUpdate* upd = (AsyncWeatherUpdate*) heap_caps_malloc(sizeof(AsyncWeatherUpdate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!upd) {
        upd = (AsyncWeatherUpdate*) heap_caps_malloc(sizeof(AsyncWeatherUpdate), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!upd) {
        ESP_LOGE(TAG, "Failed to allocate AsyncWeatherUpdate");
        return;
    }

    // Copy only minimally required data
    memcpy(&upd->data, data, sizeof(weather_data_t));
    upd->count = 0;
    if (forecast && count > 0) {
        uint8_t c = (count > 4) ? 4 : count;
        memcpy(upd->forecast, forecast, sizeof(weather_forecast_day_t) * c);
        upd->count = c;
    }

    // Schedule execution in LVGL context
    // lv_async_call executes weather_update_async_cb(user_data) in the LVGL task
    if (!lvgl_port_lock(-1)) {
        heap_caps_free(upd);
        return;
    }
    lv_res_t rc = lv_async_call(weather_update_async_cb, (void*)upd);
    lvgl_port_unlock();
    if (rc != LV_RES_OK) {
        heap_caps_free(upd);
    }
}

static void widget_click_callback(void* user_data) {
    ESP_LOGI(TAG, "Weather widget clicked - showing fullscreen UI");

    WeatherUI* ui = WeatherUI::get_instance();

    weather_data_t current_data;
    weather_forecast_day_t forecast[4];
    uint8_t forecast_count = 0;

    if (weather_integration_get_weather(&current_data)) {
        ui->update_current_weather(&current_data);
        if (weather_integration_get_forecast(forecast, &forecast_count) && forecast_count > 0) {
            ui->update_forecast(forecast, forecast_count);
        }
    } else {
        ESP_LOGW(TAG, "No weather data available for UI");
    }

    ui->show();
}

void weather_integration_show_fullscreen(void)
{
    if (!integration_initialized) {
        weather_integration_init();
    }

    WeatherUI* ui = WeatherUI::get_instance();
    if (ui) {
        ui->show();
    }
}

static void normalize_city_name(char* dst, const char* src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src || src[0] == '\0') { dst[0] = '\0'; return; }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    size_t len = strlen(dst);
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)dst[i];
        if (i == 0) dst[i] = (char)toupper(ch);
        else dst[i] = (char)tolower(ch);
    }

    if (len >= 3 && (strncmp(dst, "St ", 3) == 0 || strncmp(dst, "st ", 3) == 0)) {
        char temp[150] = {0};
        snprintf(temp, sizeof(temp) - 1, "Saint %s", dst + 3);
        strncpy(dst, temp, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static void load_persisted_weather_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kWeatherConfigNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    size_t api_key_len = sizeof(current_config.api_key);
    if (nvs_get_str(handle, kWeatherApiKeyKey, current_config.api_key, &api_key_len) != ESP_OK) {
        current_config.api_key[0] = '\0';
    }

    uint8_t metric_units = current_config.metric_units ? 1 : 0;
    if (nvs_get_u8(handle, kWeatherUnitsKey, &metric_units) == ESP_OK) {
        current_config.metric_units = (metric_units != 0);
    }

    nvs_close(handle);
}

static void save_persisted_weather_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kWeatherConfigNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open weather settings namespace: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, kWeatherApiKeyKey, current_config.api_key);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kWeatherUnitsKey, current_config.metric_units ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist weather settings: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
}

static void apply_current_config_to_module(void) {
    if ((current_config.city[0] != '\0') || (current_config.latitude != 0.0 || current_config.longitude != 0.0)) {
        weather_module_set_config(&current_config);
        ESP_LOGI(TAG, "Weather module config applied: city='%s' coords=%f,%f use_coords=%d",
                 current_config.city, current_config.latitude, current_config.longitude, current_config.use_coordinates ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "No valid location in current_config; not applying to weather module");
    }
}

static bool weather_config_has_location(void)
{
    return ((current_config.latitude != 0.0 || current_config.longitude != 0.0) ||
            current_config.city[0] != '\0');
}

/* Forward decl for start task */
static void weather_start_task(void *arg);
static void weather_start_from_ip_event_task(void *arg);

static void weather_start_from_ip_event_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Deferred IP_EVENT weather start");
    weather_integration_start();
    s_ip_start_deferred_task = NULL;
    vTaskDelete(NULL);
}

/* IP event handler: called when interface gets address */
static void ip_got_ip_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)base; (void)id; (void)data;
    if (s_ip_start_deferred_task != NULL) {
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP: deferred weather start already queued");
        return;
    }
    if (weather_module_is_running() || s_start_task_running) {
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP: weather already running or starting");
        return;
    }

    BaseType_t rc = xTaskCreate(
        weather_start_from_ip_event_task,
        "weather_ip_start",
        kWeatherIpStartTaskStackBytes,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_ip_start_deferred_task);

    if (rc != pdPASS) {
        s_ip_start_deferred_task = NULL;
        ESP_LOGW(TAG, "IP_EVENT_STA_GOT_IP: failed to queue deferred weather start");
    }
}

/* Initialize integration: weather module init + register IP event */
void weather_integration_init(void) {
    if (integration_initialized) return;

    ESP_LOGI(TAG, "Initializing weather integration...");

    weather_module_init();

    memset(&current_config, 0, sizeof(current_config));
    current_config.latitude = 0.0;
    current_config.longitude = 0.0;
    current_config.city[0] = '\0';
    current_config.metric_units = true;
    current_config.update_interval_sec = 300;
    current_config.use_coordinates = false;
    load_persisted_weather_settings();
    weather_module_set_config(&current_config);

    weather_module_set_update_callback(weather_data_update_callback);

    esp_err_t err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            ip_got_ip_handler, nullptr, &s_ip_event_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register IP_EVENT handler: %d", err);
        s_ip_event_handle = NULL;
    } else {
        ESP_LOGI(TAG, "IP_EVENT handler registered for GOT_IP");
    }

    integration_initialized = true;
    ESP_LOGI(TAG, "Weather integration initialized (no default location)");
}

/* widget create (unchanged) */
lv_obj_t* weather_integration_create_widget(lv_obj_t* parent) {
    if (!parent) {
        ESP_LOGE(TAG, "Cannot create widget: parent is NULL");
        return NULL;
    }

    ESP_LOGI(TAG, "Creating weather widget...");

    if (!integration_initialized) {
        weather_integration_init();
    }

    if (weather_widget) {
        lv_obj_t* existing_obj = weather_widget_get_obj(weather_widget);
        if (existing_obj && lv_obj_is_valid(existing_obj) && lv_obj_get_parent(existing_obj) == parent) {
            weather_data_t cached_data;
            if (weather_integration_get_weather(&cached_data)) {
                weather_widget_update(weather_widget, &cached_data);
            }
            weather_integration_start();
            ESP_LOGI(TAG, "Weather widget already exists on this screen, reusing");
            return existing_obj;
        }
        weather_widget_delete(weather_widget);
        weather_widget = NULL;
    }

    weather_widget = weather_widget_create(parent);
    if (!weather_widget) {
        ESP_LOGE(TAG, "Failed to create weather widget");
        return NULL;
    }

    weather_widget_set_click_callback(weather_widget, widget_click_callback, NULL);

    weather_data_t data;
    if (weather_integration_get_weather(&data)) {
        weather_widget_update(weather_widget, &data);
    } else {
        memset(&data, 0, sizeof(data));
        strncpy(data.city, language_get_en() ? "Loading..." : "正在加载...", sizeof(data.city) - 1);
        strncpy(data.description, language_get_en() ? "Waiting for data..." : "等待数据...", sizeof(data.description) - 1);
        strncpy(data.icon, "01d", sizeof(data.icon) - 1);
        weather_widget_update(weather_widget, &data);
    }

    weather_integration_start();

    ESP_LOGI(TAG, "Weather widget created successfully");
    return weather_widget_get_obj(weather_widget);
}

void weather_integration_refresh_language(void) {
    if (weather_widget) {
        weather_widget_refresh_language(weather_widget);
    }

    weather_module_request_update();

    WeatherUI* ui = WeatherUI::get_instance();
    if (ui && ui->is_initialized()) {
        ui->refresh_language();
        ui->set_time_synced(time(NULL) > 1000000);
    }
}

void weather_integration_destroy_widget(void) {
    if (weather_widget) {
        weather_widget_delete(weather_widget);
        weather_widget = NULL;
    }
}

void weather_integration_widget_set_pos(lv_obj_t* widget, int32_t x, int32_t y) {
    lv_obj_set_pos(widget, x, y);
}

void weather_integration_widget_set_size(lv_obj_t* widget, int32_t width, int32_t height) {
    lv_obj_set_size(widget, width, height);
}

/* weather_start_task: waits for location/time and starts the module (robust) */
static void weather_start_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "weather_start_task: background start task running");

    const int max_attempts = 30; // ~30s total
    const int delay_ms = 1000;
    bool have_location = false;
    double lat = 0.0, lon = 0.0;
    char city[128] = {0};
    char tz[64] = {0};

    for (int i = 0; i < max_attempts && !s_start_task_cancel; ++i) {
        if ((current_config.use_coordinates && (current_config.latitude != 0.0 || current_config.longitude != 0.0)) ||
            (!current_config.use_coordinates && current_config.city[0] != '\0')) {
            lat = current_config.latitude;
            lon = current_config.longitude;
            strncpy(city, current_config.city, sizeof(city) - 1);
            if (location_service_get_timezone(tz, sizeof(tz))) {
                ESP_LOGI(TAG, "weather_start_task: timezone: %s", tz);
            }
            have_location = true;
            break;
        }

        if (location_service_get(&lat, &lon, city, sizeof(city))) {
            ESP_LOGI(TAG, "weather_start_task: got cached location: '%s' (%f,%f)", city, lat, lon);
            if (location_service_get_timezone(tz, sizeof(tz))) {
                ESP_LOGI(TAG, "weather_start_task: timezone: %s", tz);
            }
            have_location = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (s_start_task_cancel) {
        ESP_LOGI(TAG, "weather_start_task: cancelled before completion");
        s_start_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!have_location) {
        ESP_LOGW(TAG, "weather_start_task: no valid location obtained within timeout — NOT starting weather module");
        s_start_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (lat != 0.0 || lon != 0.0) {
        current_config.latitude = lat;
        current_config.longitude = lon;
        current_config.use_coordinates = true;
        current_config.city[0] = '\0';
    } else if (city[0] != '\0') {
        normalize_city_name(current_config.city, city, sizeof(current_config.city));
        current_config.use_coordinates = false;
        current_config.latitude = 0.0;
        current_config.longitude = 0.0;
    }

    apply_current_config_to_module();

    if (!time_sync_is_synced()) {
        ESP_LOGI(TAG, "weather_start_task: waiting for external time sync before first fetch...");
        int waited = 0;
        const int max_wait = 30; // seconds
        while (waited < max_wait && !time_sync_is_synced() && !s_start_task_cancel) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
        }
        if (!time_sync_is_synced()) {
            ESP_LOGW(TAG, "weather_start_task: time not synced after wait — proceeding anyway");
        } else {
            ESP_LOGI(TAG, "weather_start_task: time synced");
        }
    }

    if (s_start_task_cancel) {
        ESP_LOGI(TAG, "weather_start_task: cancelled before starting module");
        s_start_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (weather_module_is_running()) {
        ESP_LOGI(TAG, "weather_start_task: weather_module already running; config applied");
    } else {
        weather_module_start();
        ESP_LOGI(TAG, "weather_start_task: weather module started");
    }

    s_start_task_running = false;
    vTaskDelete(NULL);
}

/* Set location explicitly */
void weather_integration_set_location(double lat, double lon, const char* city) {
    if (!integration_initialized) {
        weather_integration_init();
    }

    ESP_LOGI(TAG, "Setting location: %s (%.6f, %.6f)", city ? city : "(none)", lat, lon);

    if (lat != 0.0 || lon != 0.0) {
        current_config.latitude = lat;
        current_config.longitude = lon;
        current_config.use_coordinates = true;
        current_config.city[0] = '\0';
    } else if (city && city[0] != '\0') {
        normalize_city_name(current_config.city, city, sizeof(current_config.city));
        current_config.use_coordinates = false;
        current_config.latitude = 0.0;
        current_config.longitude = 0.0;
    } else {
        current_config.city[0] = '\0';
        current_config.latitude = 0.0;
        current_config.longitude = 0.0;
        current_config.use_coordinates = false;
    }

    apply_current_config_to_module();
}

/* Start weather integration (non-blocking) */
void weather_integration_start(void) {
    if (!integration_initialized) {
        weather_integration_init();
    }

    ESP_LOGI(TAG, "Requested weather_integration_start()");

    if (!weather_config_has_location()) {
        ESP_LOGI(TAG, "Weather start deferred: location is not ready yet");
        return;
    }

    if (weather_module_is_running()) {
        ESP_LOGI(TAG, "Weather module already running; ensuring config is up to date");
        apply_current_config_to_module();
        return;
    }

    if (s_start_task_running) {
        ESP_LOGI(TAG, "Weather start task already running; returning (config applied)");
        apply_current_config_to_module();
        return;
    }

    s_start_task_cancel = false;
    s_start_task_running = true;
    BaseType_t r = xTaskCreate(weather_start_task, "weather_start", 6 * 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (r != pdPASS) {
        s_start_task_running = false;
        ESP_LOGE(TAG, "Failed to create weather_start_task (%d)", (int)r);
    } else {
        ESP_LOGI(TAG, "weather_start_task created");
    }
}

/* Stop integration / cleanup */
void weather_integration_stop(void) {
    ESP_LOGI(TAG, "Stopping weather integration...");

    if (s_start_task_running) {
        s_start_task_cancel = true;
        for (int i = 0; i < 20 && s_start_task_running; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (weather_module_is_running()) {
        weather_module_stop();
    }

    WeatherUI::destroy_instance();

    if (weather_widget) {
        weather_widget_delete(weather_widget);
        weather_widget = NULL;
    }

    if (s_ip_event_handle) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_handle);
        s_ip_event_handle = NULL;
    }
}

bool weather_integration_is_running(void) {
    return weather_module_is_running() || s_start_task_running;
}

void weather_integration_set_api_key(const char* api_key) {
    if (!integration_initialized) {
        weather_integration_init();
    }

    if (api_key) {
        strncpy(current_config.api_key, api_key, sizeof(current_config.api_key) - 1);
        current_config.api_key[sizeof(current_config.api_key) - 1] = '\0';
        save_persisted_weather_settings();
        weather_module_set_config(&current_config);
        weather_module_request_update();
        ESP_LOGI(TAG, "API key updated");
    }
}

void weather_integration_get_api_key_copy(char* api_key, size_t api_key_size)
{
    if (!api_key || api_key_size == 0) {
        return;
    }

    if (!integration_initialized) {
        weather_integration_init();
    }

    strncpy(api_key, current_config.api_key, api_key_size - 1);
    api_key[api_key_size - 1] = '\0';
}

void weather_integration_set_metric_units(bool metric_units)
{
    if (!integration_initialized) {
        weather_integration_init();
    }

    if (current_config.metric_units == metric_units) {
        return;
    }

    weather_module_convert_cached_units(metric_units);
    current_config.metric_units = metric_units;
    save_persisted_weather_settings();
    weather_module_set_config(&current_config);
    weather_module_request_update();
    ESP_LOGI(TAG, "Weather units updated: %s", metric_units ? "metric" : "imperial");
}

bool weather_integration_get_metric_units(void)
{
    if (!integration_initialized) {
        weather_integration_init();
    }

    return current_config.metric_units;
}

void weather_integration_show_ui(void) {
    ESP_LOGI(TAG, "Showing weather UI");
    weather_integration_show_fullscreen();
}

void weather_integration_hide_ui(void) {
    ESP_LOGI(TAG, "Hiding weather UI");
    WeatherUI* ui = WeatherUI::get_instance();
    if (ui && ui->is_initialized()) {
        ui->hide();
    }
}

bool weather_integration_get_weather(weather_data_t* data) {
    return weather_get_current_data(data);
}

bool weather_integration_get_forecast(weather_forecast_day_t* forecast, uint8_t* count) {
    return weather_get_forecast_data(forecast, count);
}
