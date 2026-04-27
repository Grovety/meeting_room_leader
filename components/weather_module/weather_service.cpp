// weather_service.cpp - optimized single-pass forecast parser, with yields and SPIRAM usage
#include "weather_service.h"
#include "language_manager.h"
#include "network_request_guard.h"
#include "cJSON.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <json_parser.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "weather_service";
static volatile int s_last_http_status = 0;

class ScopedNetworkRequestLock {
public:
    explicit ScopedNetworkRequestLock(uint32_t timeout_ms)
        : locked_(network_request_guard_lock(timeout_ms))
    {
    }

    ~ScopedNetworkRequestLock()
    {
        if (locked_) {
            network_request_guard_unlock();
        }
    }

    bool locked() const { return locked_; }

private:
    bool locked_;
};

/* Retry settings */
static const int WEATHER_HTTP_MAX_RETRIES = 3;
static const int WEATHER_HTTP_BACKOFF_MS[] = { 1000, 2000, 5000 }; // ms
static const size_t WEATHER_HTTP_MIN_INTERNAL_FREE = 5 * 1024;
static const size_t WEATHER_HTTP_MIN_INTERNAL_LARGEST = 2 * 1024;
static const int WEATHER_HTTP_HEAP_WAIT_ROUNDS = 8;
static const int WEATHER_HTTP_HEAP_WAIT_MS = 250;

struct ForecastPoint {
    time_t timestamp;
    float temp;
    float feels_like;
    float pop;
    float rain;
    float snow;
    float humidity;
    float pressure;
    float wind_speed;
    float wind_direction;
    bool is_night;
    bool is_day;
    char description[64];
    char icon[8];
};

static bool weather_wait_for_http_heap(const char* stage) {
    for (int attempt = 0; attempt < WEATHER_HTTP_HEAP_WAIT_ROUNDS; ++attempt) {
        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (free_internal >= WEATHER_HTTP_MIN_INTERNAL_FREE &&
            largest_internal >= WEATHER_HTTP_MIN_INTERNAL_LARGEST) {
            return true;
        }

        if (attempt == 0 || attempt == WEATHER_HTTP_HEAP_WAIT_ROUNDS - 1) {
            ESP_LOGW(TAG,
                     "HTTP heap guard (%s): INTERNAL free=%llu largest=%llu, need at least free=%u largest=%u",
                     stage ? stage : "?",
                     (unsigned long long)free_internal,
                     (unsigned long long)largest_internal,
                     (unsigned)WEATHER_HTTP_MIN_INTERNAL_FREE,
                     (unsigned)WEATHER_HTTP_MIN_INTERNAL_LARGEST);
        }

        vTaskDelay(pdMS_TO_TICKS(WEATHER_HTTP_HEAP_WAIT_MS));
    }

    return false;
}

static void clear_today_outlook(weather_today_outlook_t* outlook) {
    if (!outlook) {
        return;
    }

    memset(outlook, 0, sizeof(*outlook));
}

static void populate_today_outlook_from_points(const ForecastPoint* points,
                                               int point_count,
                                               time_t now,
                                               weather_today_outlook_t* outlook) {
    if (!outlook) {
        return;
    }

    clear_today_outlook(outlook);
    if (!points || point_count <= 0) {
        return;
    }

    struct tm today_tm;
    localtime_r(&now, &today_tm);
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    time_t today_midnight = mktime(&today_tm);
    time_t tomorrow_midnight = today_midnight + 86400;

    bool has_today_points = false;
    bool has_future_points = false;
    for (int i = 0; i < point_count; ++i) {
        time_t pt = points[i].timestamp;
        if (pt < now) {
            continue;
        }

        has_future_points = true;

        if (pt < today_midnight || pt >= tomorrow_midnight) {
            continue;
        }

        has_today_points = true;

        if (points[i].pop > outlook->pop) {
            outlook->pop = points[i].pop;
        }
        outlook->rain += points[i].rain;
        outlook->snow += points[i].snow;

        if (outlook->precipitation_time == 0 &&
            (points[i].pop > 0.0f || points[i].rain > 0.0f || points[i].snow > 0.0f)) {
            outlook->precipitation_time = static_cast<uint32_t>(pt);
            if (points[i].rain > 0.0f && points[i].snow > 0.0f) {
                strncpy(outlook->precipitation_type, "mixed", sizeof(outlook->precipitation_type) - 1);
            } else if (points[i].snow > 0.0f) {
                strncpy(outlook->precipitation_type, "snow", sizeof(outlook->precipitation_type) - 1);
            } else {
                strncpy(outlook->precipitation_type, "rain", sizeof(outlook->precipitation_type) - 1);
            }
            outlook->precipitation_type[sizeof(outlook->precipitation_type) - 1] = '\0';
        }
    }

    if (!has_today_points) {
        if (has_future_points) {
            outlook->valid = true;
            ESP_LOGI(TAG, "Today outlook: no forecast points remain for today, treating remainder as dry");
        }
        return;
    }

    outlook->valid = true;
    ESP_LOGI(TAG, "Today outlook: POP %.0f%%, rain %.1f mm, snow %.1f mm, next=%u",
             outlook->pop * 100.0f, outlook->rain, outlook->snow, (unsigned)outlook->precipitation_time);
}

static ForecastPoint* allocate_forecast_points(size_t max_points, bool* points_in_spiram) {
    if (points_in_spiram) {
        *points_in_spiram = true;
    }

    ForecastPoint* points = static_cast<ForecastPoint*>(
        heap_caps_malloc(sizeof(ForecastPoint) * max_points, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!points) {
        points = static_cast<ForecastPoint*>(
            heap_caps_malloc(sizeof(ForecastPoint) * max_points, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (points_in_spiram) {
            *points_in_spiram = false;
        }
    }
    return points;
}

static bool json_get_number(cJSON* object, const char* key, float* out) {
    if (!object || !key || !out) {
        return false;
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = static_cast<float>(item->valuedouble);
    return true;
}

static bool json_get_number_i64(cJSON* object, const char* key, int64_t* out) {
    if (!object || !key || !out) {
        return false;
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = static_cast<int64_t>(item->valuedouble);
    return true;
}

static void json_copy_string(cJSON* object, const char* key, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!object || !key) {
        return;
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static uint8_t aggregate_forecast_points(const ForecastPoint* points,
                                         int point_count,
                                         time_t today_midnight,
                                         weather_forecast_day_t* forecast) {
    struct DayInfo {
        time_t day_start;
        float day_temp;
        float night_temp;
        float feels_like_day;
        char description[64];
        char icon[8];
        float max_pop;
        float total_rain;
        float total_snow;
        float humidity;
        float pressure;
        float wind_speed;
        float wind_direction;
        uint32_t next_precip_time;
        float next_precip_pop;
        char next_precip_type[16];
        bool has_day_feels_like;
        bool has_data;
    };

    DayInfo day_infos[3];
    for (int d = 0; d < 3; ++d) {
        day_infos[d].day_start = today_midnight + ((d + 1) * 86400);
        day_infos[d].day_temp = -1000.0f;
        day_infos[d].night_temp = 1000.0f;
        day_infos[d].feels_like_day = 0.0f;
        day_infos[d].description[0] = '\0';
        day_infos[d].icon[0] = '\0';
        day_infos[d].max_pop = 0.0f;
        day_infos[d].total_rain = 0.0f;
        day_infos[d].total_snow = 0.0f;
        day_infos[d].humidity = 0.0f;
        day_infos[d].pressure = 0.0f;
        day_infos[d].wind_speed = 0.0f;
        day_infos[d].wind_direction = 0.0f;
        day_infos[d].next_precip_time = 0;
        day_infos[d].next_precip_pop = 0.0f;
        day_infos[d].next_precip_type[0] = '\0';
        day_infos[d].has_day_feels_like = false;
        day_infos[d].has_data = false;
    }

    for (int i = 0; i < point_count; ++i) {
        if ((i & 7) == 0) {
            taskYIELD();
        }

        time_t pt = points[i].timestamp;
        for (int d = 0; d < 3; ++d) {
            time_t start = day_infos[d].day_start;
            if (pt < start || pt >= start + 86400) {
                continue;
            }

            day_infos[d].has_data = true;
            if (points[i].is_day && points[i].temp > day_infos[d].day_temp) {
                day_infos[d].day_temp = points[i].temp;
                day_infos[d].feels_like_day = points[i].feels_like;
                day_infos[d].has_day_feels_like = true;
            }
            if (points[i].is_night && points[i].temp < day_infos[d].night_temp) {
                day_infos[d].night_temp = points[i].temp;
            }
            if (points[i].pop > day_infos[d].max_pop) {
                day_infos[d].max_pop = points[i].pop;
            }
            day_infos[d].total_rain += points[i].rain;
            day_infos[d].total_snow += points[i].snow;

            if (day_infos[d].next_precip_time == 0 &&
                (points[i].pop > 0.0f || points[i].rain > 0.0f || points[i].snow > 0.0f)) {
                day_infos[d].next_precip_time = static_cast<uint32_t>(pt);
                day_infos[d].next_precip_pop = points[i].pop;
                if (points[i].rain > 0.0f && points[i].snow > 0.0f) {
                    strncpy(day_infos[d].next_precip_type, "mixed", sizeof(day_infos[d].next_precip_type) - 1);
                } else if (points[i].snow > 0.0f) {
                    strncpy(day_infos[d].next_precip_type, "snow", sizeof(day_infos[d].next_precip_type) - 1);
                } else {
                    strncpy(day_infos[d].next_precip_type, "rain", sizeof(day_infos[d].next_precip_type) - 1);
                }
                day_infos[d].next_precip_type[sizeof(day_infos[d].next_precip_type) - 1] = '\0';
            }

            struct tm ptm;
            localtime_r(&pt, &ptm);
            if (ptm.tm_hour >= 12 && ptm.tm_hour <= 14) {
                if (points[i].description[0]) {
                    strncpy(day_infos[d].description, points[i].description, sizeof(day_infos[d].description) - 1);
                    day_infos[d].description[sizeof(day_infos[d].description) - 1] = '\0';
                }
                if (points[i].icon[0]) {
                    strncpy(day_infos[d].icon, points[i].icon, sizeof(day_infos[d].icon) - 1);
                    day_infos[d].icon[sizeof(day_infos[d].icon) - 1] = '\0';
                }
            }

            if (day_infos[d].humidity == 0.0f && points[i].humidity != 0.0f) {
                day_infos[d].humidity = points[i].humidity;
            }
            if (day_infos[d].pressure == 0.0f && points[i].pressure != 0.0f) {
                day_infos[d].pressure = points[i].pressure;
            }
            if (day_infos[d].wind_speed == 0.0f && points[i].wind_speed != 0.0f) {
                day_infos[d].wind_speed = points[i].wind_speed;
            }
            if (day_infos[d].wind_direction == 0.0f && points[i].wind_direction != 0.0f) {
                day_infos[d].wind_direction = points[i].wind_direction;
            }
            break;
        }
    }

    for (int d = 0; d < 3; ++d) {
        if (!day_infos[d].has_data) {
            continue;
        }

        if (day_infos[d].day_temp <= -1000.0f) {
            float max_temp = -1000.0f;
            float max_feels_like = 0.0f;
            for (int i = 0; i < point_count; ++i) {
                time_t pt = points[i].timestamp;
                if (pt < day_infos[d].day_start || pt >= day_infos[d].day_start + 86400) {
                    continue;
                }
                if (points[i].temp > max_temp) {
                    max_temp = points[i].temp;
                    max_feels_like = points[i].feels_like;
                }
            }
            if (max_temp > -1000.0f) {
                day_infos[d].day_temp = max_temp;
                day_infos[d].feels_like_day = max_feels_like;
                day_infos[d].has_day_feels_like = true;
            }
        }

        if (day_infos[d].night_temp >= 1000.0f) {
            float min_temp = 1000.0f;
            for (int i = 0; i < point_count; ++i) {
                time_t pt = points[i].timestamp;
                if (pt < day_infos[d].day_start || pt >= day_infos[d].day_start + 86400) {
                    continue;
                }
                if (points[i].temp < min_temp) {
                    min_temp = points[i].temp;
                }
            }
            if (min_temp < 1000.0f) {
                day_infos[d].night_temp = min_temp;
            } else {
                day_infos[d].night_temp = day_infos[d].day_temp - 5.0f;
            }
        }
    }

    int days_added = 0;
    for (int d = 0; d < 3 && days_added < 3; ++d) {
        if (!day_infos[d].has_data) {
            continue;
        }

        weather_forecast_day_t* out = &forecast[days_added];
        memset(out, 0, sizeof(*out));
        out->timestamp = static_cast<uint32_t>(day_infos[d].day_start);
        out->temp_day = day_infos[d].day_temp;
        out->temp_night = day_infos[d].night_temp;
        out->feels_like_day = day_infos[d].has_day_feels_like ? day_infos[d].feels_like_day : day_infos[d].day_temp;
        out->pop = day_infos[d].max_pop;
        out->rain = day_infos[d].total_rain;
        out->snow = day_infos[d].total_snow;
        out->humidity = day_infos[d].humidity;
        out->pressure = day_infos[d].pressure;
        out->wind_speed = day_infos[d].wind_speed;
        out->wind_direction = day_infos[d].wind_direction;
        out->next_precip_time = day_infos[d].next_precip_time;
        out->next_precip_pop = day_infos[d].next_precip_pop;

        strncpy(out->description,
                day_infos[d].description[0] ? day_infos[d].description : "Clear sky",
                sizeof(out->description) - 1);
        out->description[sizeof(out->description) - 1] = '\0';
        strncpy(out->icon,
                day_infos[d].icon[0] ? day_infos[d].icon : "01d",
                sizeof(out->icon) - 1);
        out->icon[sizeof(out->icon) - 1] = '\0';
        if (day_infos[d].next_precip_type[0]) {
            strncpy(out->next_precip_type, day_infos[d].next_precip_type, sizeof(out->next_precip_type) - 1);
            out->next_precip_type[sizeof(out->next_precip_type) - 1] = '\0';
        }

        struct tm day_tm;
        time_t day_time = static_cast<time_t>(out->timestamp);
        localtime_r(&day_time, &day_tm);
        ESP_LOGI(TAG, "Forecast day %d: %02d.%02d.%04d, Day: %.1f°C, Night: %.1f°C, POP: %.0f%%",
                 days_added + 1, day_tm.tm_mday, day_tm.tm_mon + 1, day_tm.tm_year + 1900,
                 out->temp_day, out->temp_night, out->pop * 100.0f);

        ++days_added;
    }

    return static_cast<uint8_t>(days_added);
}

static bool parse_forecast_with_static_tokens(const char* json,
                                              size_t len,
                                              weather_forecast_day_t* forecast,
                                              uint8_t* count,
                                              weather_today_outlook_t* today_outlook) {
    static constexpr int kForecastTokenCount = 4096;

    json_tok_t* tokens = static_cast<json_tok_t*>(
        heap_caps_malloc(sizeof(json_tok_t) * kForecastTokenCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool tokens_in_spiram = true;
    if (!tokens) {
        tokens = static_cast<json_tok_t*>(
            heap_caps_malloc(sizeof(json_tok_t) * kForecastTokenCount, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        tokens_in_spiram = false;
    }
    if (!tokens) {
        ESP_LOGE(TAG, "Failed to allocate forecast token buffer");
        return false;
    }

    jparse_ctx_t jctx;
    if (json_parse_start_static(&jctx, json, (int)len, tokens, kForecastTokenCount) != OS_SUCCESS) {
        ESP_LOGE(TAG, "json_parse_start_static failed for forecast (tokens=%d, storage=%s)",
                 kForecastTokenCount, tokens_in_spiram ? "SPIRAM" : "INTERNAL");
        heap_caps_free(tokens);
        return false;
    }

    bool success = false;
    ForecastPoint* points = nullptr;

    do {
        time_t now;
        time(&now);
        struct tm today_tm;
        localtime_r(&now, &today_tm);
        today_tm.tm_hour = 0;
        today_tm.tm_min = 0;
        today_tm.tm_sec = 0;
        time_t today_midnight = mktime(&today_tm);

        int array_size = 0;
        if (json_obj_get_array(&jctx, "list", &array_size) != OS_SUCCESS) {
            ESP_LOGE(TAG, "No list array in forecast response");
            break;
        }
        ESP_LOGI(TAG, "Forecast list array size: %d", array_size);

        if (json_obj_get_object(&jctx, "city") == OS_SUCCESS) {
            json_obj_leave_object(&jctx);
        }

        int max_points = (array_size < 40) ? array_size : 40;
        bool points_in_spiram = true;
        points = allocate_forecast_points((size_t)max_points, &points_in_spiram);
        if (!points) {
            ESP_LOGE(TAG, "Failed to allocate forecast points array");
            break;
        }
        memset(points, 0, sizeof(ForecastPoint) * (size_t)max_points);

        int point_count = 0;
        for (int i = 0; i < array_size && point_count < max_points; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1));

            if (json_arr_get_object(&jctx, i) != OS_SUCCESS) {
                break;
            }

            int64_t dt = 0;
            if (json_obj_get_int64(&jctx, "dt", &dt) != OS_SUCCESS) {
                json_arr_leave_object(&jctx);
                continue;
            }

            ForecastPoint* point = &points[point_count];
            point->timestamp = static_cast<time_t>(dt);

            if (json_obj_get_object(&jctx, "main") == OS_SUCCESS) {
                json_obj_get_float(&jctx, "temp", &point->temp);
                json_obj_get_float(&jctx, "feels_like", &point->feels_like);
                json_obj_get_float(&jctx, "humidity", &point->humidity);
                json_obj_get_float(&jctx, "pressure", &point->pressure);
                json_obj_leave_object(&jctx);
            }

            json_obj_get_float(&jctx, "pop", &point->pop);

            if (json_obj_get_object(&jctx, "rain") == OS_SUCCESS) {
                float rain = 0.0f;
                if (json_obj_get_float(&jctx, "3h", &rain) == OS_SUCCESS) {
                    point->rain = rain;
                }
                json_obj_leave_object(&jctx);
            }

            if (json_obj_get_object(&jctx, "snow") == OS_SUCCESS) {
                float snow = 0.0f;
                if (json_obj_get_float(&jctx, "3h", &snow) == OS_SUCCESS) {
                    point->snow = snow;
                }
                json_obj_leave_object(&jctx);
            }

            int weather_count = 0;
            if (json_obj_get_array(&jctx, "weather", &weather_count) == OS_SUCCESS && weather_count > 0) {
                if (json_arr_get_object(&jctx, 0) == OS_SUCCESS) {
                    json_obj_get_string(&jctx, "description", point->description, sizeof(point->description) - 1);
                    json_obj_get_string(&jctx, "icon", point->icon, sizeof(point->icon) - 1);
                    if (point->description[0]) {
                        point->description[0] = (char)toupper((unsigned char)point->description[0]);
                    }
                    json_arr_leave_object(&jctx);
                }
                json_obj_leave_array(&jctx);
            }

            if (json_obj_get_object(&jctx, "wind") == OS_SUCCESS) {
                json_obj_get_float(&jctx, "speed", &point->wind_speed);
                json_obj_get_float(&jctx, "deg", &point->wind_direction);
                json_obj_leave_object(&jctx);
            }

            struct tm point_tm;
            localtime_r(&point->timestamp, &point_tm);
            int hour = point_tm.tm_hour;
            point->is_night = (hour >= 21 || hour <= 6);
            point->is_day = (hour >= 11 && hour <= 14);

            ++point_count;
            json_arr_leave_object(&jctx);
        }

        json_obj_leave_array(&jctx);

        populate_today_outlook_from_points(points, point_count, now, today_outlook);
        *count = aggregate_forecast_points(points, point_count, today_midnight, forecast);
        success = (*count > 0);
    } while (0);

    if (points) {
        heap_caps_free(points);
    }
    json_parse_end_static(&jctx);
    heap_caps_free(tokens);

    if (!success) {
        ESP_LOGW(TAG, "Static-token forecast parser produced no forecast days");
    }
    return success;
}

static uint32_t perform_http_once(const char* log_tag,
                                  const char* url,
                                  char* response_buffer,
                                  size_t response_buffer_size) {
    s_last_http_status = 0;
    if (!url || !response_buffer || response_buffer_size < 2) {
        ESP_LOGE(log_tag, "Invalid HTTP request arguments");
        return 0;
    }

    ScopedNetworkRequestLock request_lock(25000);
    if (!request_lock.locked()) {
        ESP_LOGW(log_tag, "Skipping HTTP attempt: another network request is already in progress");
        return 0;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 20000;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    const bool is_https = (strncmp(url, "https://", 8) == 0);
    config.addr_type = HTTP_ADDR_TYPE_INET;
    if (is_https) {
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }

    if (!weather_wait_for_http_heap("before_init")) {
        ESP_LOGW(log_tag, "Skipping HTTP attempt for now: not enough INTERNAL heap before client init");
        return 0;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(log_tag, "Failed to init esp_http_client");
        return 0;
    }

    esp_http_client_set_header(client, "Accept", "application/json");

    if (!weather_wait_for_http_heap("before_open")) {
        if (is_https) {
            ESP_LOGW(log_tag, "Skipping HTTP attempt for now: not enough INTERNAL heap before open");
            esp_http_client_cleanup(client);
            return 0;
        }
        ESP_LOGW(log_tag, "Low INTERNAL heap before open, but continuing because request is plain HTTP");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "esp_http_client_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return 0;
    }

    int64_t header_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    s_last_http_status = status;
    int content_len = esp_http_client_get_content_length(client);
    ESP_LOGI(log_tag, "HTTP status=%d, content_length=%d, header_len=%" PRId64, status, content_len, header_len);

    if (header_len < 0) {
        ESP_LOGW(log_tag, "Failed to fetch HTTP headers");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return 0;
    }

    if (content_len > 0 && (size_t)content_len >= response_buffer_size) {
        ESP_LOGE(log_tag, "HTTP response too large: %d bytes (buf %u)", content_len, (unsigned)response_buffer_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return 0;
    }

    size_t total_read = 0;
    while (total_read < response_buffer_size - 1) {
        int read_len = esp_http_client_read(
            client,
            response_buffer + total_read,
            response_buffer_size - 1 - total_read);
        if (read_len < 0) {
            ESP_LOGE(log_tag, "esp_http_client_read failed");
            total_read = 0;
            break;
        }
        if (read_len == 0) {
            break;
        }
        total_read += (size_t)read_len;
    }

    response_buffer[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(log_tag, "HTTP request returned status %d", status);
        if (total_read > 0) {
            ESP_LOGW(log_tag, "HTTP error body prefix: %.*s", (int)((total_read > 120) ? 120 : total_read), response_buffer);
        }
        return 0;
    }

    if (total_read == response_buffer_size - 1) {
        ESP_LOGW(log_tag, "HTTP response filled the entire buffer (%u bytes)", (unsigned)total_read);
    }

    return (uint32_t)total_read;
}

int weather_service_get_last_http_status(void) {
    return s_last_http_status;
}

static uint32_t perform_http_with_retries(const char* log_tag,
                                          const char* url,
                                          char* response_buffer,
                                          size_t response_buffer_size) {
    for (int attempt = 0; attempt < WEATHER_HTTP_MAX_RETRIES; ++attempt) {
        ESP_LOGI(log_tag, "HTTP attempt %d started", attempt + 1);
        uint32_t response_len = perform_http_once(log_tag, url, response_buffer, response_buffer_size);
        if (response_len > 0) {
            ESP_LOGI(log_tag, "HTTP attempt %d success, received %u bytes", attempt + 1, response_len);
            return response_len;
        } else {
            ESP_LOGW(log_tag, "HTTP attempt %d failed (response_len=%u)", attempt + 1, response_len);
            if (attempt < WEATHER_HTTP_MAX_RETRIES - 1) {
                vTaskDelay(pdMS_TO_TICKS(WEATHER_HTTP_BACKOFF_MS[attempt]));
            }
        }
    }
    ESP_LOGE(log_tag, "All %d HTTP attempts failed", WEATHER_HTTP_MAX_RETRIES);
    return 0;
}

/* ----------------- WeatherService methods ----------------- */

WeatherService::WeatherService() : ctx_(nullptr) {
    ctx_ = static_cast<Ctx*>(heap_caps_malloc(sizeof(Ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ctx_) {
        memset(ctx_, 0, sizeof(Ctx));
    } else {
        ESP_LOGE(TAG, "Failed to allocate memory for context");
    }
}

WeatherService::~WeatherService() {
    if (ctx_) {
        heap_caps_free(ctx_);
    }
}

bool WeatherService::init(const weather_config_t* config) {
    if (!ctx_ || !config) {
        ESP_LOGE(TAG, "Invalid config or context");
        return false;
    }

    strncpy(ctx_->api_key, config->api_key, sizeof(ctx_->api_key) - 1);
    ctx_->api_key[sizeof(ctx_->api_key)-1] = '\0';
    ctx_->use_coordinates = config->use_coordinates;
    ctx_->metric_units = config->metric_units;

    if (config->use_coordinates) {
        snprintf(ctx_->latitude, sizeof(ctx_->latitude), "%.6f", config->latitude);
        snprintf(ctx_->longitude, sizeof(ctx_->longitude), "%.6f", config->longitude);
        ctx_->latitude[sizeof(ctx_->latitude)-1] = '\0';
        ctx_->longitude[sizeof(ctx_->longitude)-1] = '\0';
        ESP_LOGI(TAG, "Using coordinates: lat=%s, lon=%s", ctx_->latitude, ctx_->longitude);
    } else {
        strncpy(ctx_->location_name, config->city, sizeof(ctx_->location_name) - 1);
        ctx_->location_name[sizeof(ctx_->location_name)-1] = '\0';
        ESP_LOGI(TAG, "Using city: %s", ctx_->location_name);
    }

    return true;
}

bool WeatherService::fetch_current_weather(weather_data_t* data) {
    if (!ctx_ || !data) {
        ESP_LOGE(TAG, "Invalid context or data");
        return false;
    }
    if (ctx_->api_key[0] == '\0') {
        ESP_LOGW(TAG, "Weather API key is not configured yet");
        return false;
    }

    char url[400];
    /* Keep external weather descriptions in English until dynamic CJK font support is added. */
    const char* lang = "en";
    if (ctx_->use_coordinates) {
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&appid=%s&units=%s&lang=%s",
                 ctx_->latitude, ctx_->longitude, ctx_->api_key,
                 ctx_->metric_units ? "metric" : "imperial", lang);
    } else {
        char encoded_city[256] = {0};
        url_encode(ctx_->location_name, encoded_city, sizeof(encoded_city));
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=%s&lang=%s",
                 encoded_city, ctx_->api_key,
                 ctx_->metric_units ? "metric" : "imperial", lang);
    }

    if (ctx_->use_coordinates) {
        ESP_LOGI(TAG, "Fetching current weather for coordinates lat=%s lon=%s units=%s lang=%s",
                 ctx_->latitude,
                 ctx_->longitude,
                 ctx_->metric_units ? "metric" : "imperial",
                 lang);
    } else {
        ESP_LOGI(TAG, "Fetching current weather for city='%s' units=%s lang=%s",
                 ctx_->location_name,
                 ctx_->metric_units ? "metric" : "imperial",
                 lang);
    }

    const size_t BUF_SZ = 8192;
    // Keep INTERNAL RAM available for lwIP/DNS; parsing can fall back to SPIRAM safely here.
    char* response_buffer = (char*)heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool used_spiram = response_buffer != nullptr;
    if (!response_buffer) {
        response_buffer = (char*)heap_caps_malloc(BUF_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        used_spiram = false;
    }
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for current weather");
        return false;
    }
    ESP_LOGD(TAG, "Allocated %s RAM for current weather buffer (%u bytes)",
             used_spiram ? "SPIRAM" : "INTERNAL", (unsigned)BUF_SZ);

    uint32_t response_len = perform_http_with_retries(TAG, url, response_buffer, BUF_SZ);
    bool ok = false;
    if (response_len == 0 || response_len >= (BUF_SZ - 1)) {
        ESP_LOGE(TAG, "HTTP request failed or response too large (len=%u, buf=%u)", response_len, (unsigned)(BUF_SZ - 1));
        ok = false;
    } else {
        response_buffer[response_len] = '\0';
        ESP_LOGI(TAG, "Received %u bytes for current weather", response_len);
        ok = parse_weather_response(response_buffer, response_len, data);
    }

    heap_caps_free(response_buffer);
    return ok;
}

bool WeatherService::fetch_forecast(weather_forecast_day_t* forecast, uint8_t* count,
                                    weather_today_outlook_t* today_outlook) {
    if (!ctx_ || !forecast || !count) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }
    if (ctx_->api_key[0] == '\0') {
        ESP_LOGW(TAG, "Weather API key is not configured yet");
        return false;
    }

    clear_today_outlook(today_outlook);

    char url[512];
    /* Keep external weather descriptions in English until dynamic CJK font support is added. */
    const char* lang = "en";
    if (ctx_->use_coordinates) {
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/forecast?lat=%s&lon=%s&appid=%s&units=%s&cnt=40&lang=%s",
                 ctx_->latitude, ctx_->longitude, ctx_->api_key,
                 ctx_->metric_units ? "metric" : "imperial", lang);
    } else {
        char encoded_city[256] = {0};
        url_encode(ctx_->location_name, encoded_city, sizeof(encoded_city));
        snprintf(url, sizeof(url),
                 "https://api.openweathermap.org/data/2.5/forecast?q=%s&appid=%s&units=%s&cnt=40&lang=%s",
                 encoded_city, ctx_->api_key,
                 ctx_->metric_units ? "metric" : "imperial", lang);
    }

    if (ctx_->use_coordinates) {
        ESP_LOGI(TAG, "Fetching forecast for coordinates lat=%s lon=%s units=%s lang=%s",
                 ctx_->latitude,
                 ctx_->longitude,
                 ctx_->metric_units ? "metric" : "imperial",
                 lang);
    } else {
        ESP_LOGI(TAG, "Fetching forecast for city='%s' units=%s lang=%s",
                 ctx_->location_name,
                 ctx_->metric_units ? "metric" : "imperial",
                 lang);
    }

    const size_t BUF_SZ = 20 * 1024; // 20 KiB
    // Keep INTERNAL RAM available for lwIP/DNS; parser already has an INTERNAL-copy fallback.
    char* response_buffer = (char*)heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool used_spiram = response_buffer != nullptr;
    if (!response_buffer) {
        response_buffer = (char*)heap_caps_malloc(BUF_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        used_spiram = false;
    }
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for forecast response (%u bytes)", (unsigned)BUF_SZ);
        return false;
    }
    ESP_LOGD(TAG, "Allocated %s RAM for forecast buffer (%u bytes)",
             used_spiram ? "SPIRAM" : "INTERNAL", (unsigned)BUF_SZ);

    uint32_t response_len = perform_http_with_retries(TAG, url, response_buffer, BUF_SZ);

    bool success = false;
    if (response_len == 0) {
        ESP_LOGE(TAG, "HTTP request failed for forecast");
    } else if (response_len >= (BUF_SZ - 1)) {
        ESP_LOGE(TAG, "Forecast response too large: %u bytes (buf %u)", response_len, (unsigned)(BUF_SZ - 1));
    } else {
        response_buffer[response_len] = '\0';
        ESP_LOGI(TAG, "Received %u bytes for forecast", response_len);
        ESP_LOGD(TAG, "First 200 chars: %.*s", (response_len > 200) ? 200 : (int)response_len, response_buffer);

        success = parse_forecast_response(response_buffer, response_len, forecast, count, today_outlook);
        if (!success && used_spiram) {
            char* internal_copy = static_cast<char*>(heap_caps_malloc(response_len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (internal_copy) {
                memcpy(internal_copy, response_buffer, response_len + 1);
                ESP_LOGW(TAG, "Retrying forecast parse from INTERNAL copy (%u bytes)", response_len);
                success = parse_forecast_response(internal_copy, response_len, forecast, count, today_outlook);
                heap_caps_free(internal_copy);
            } else {
                ESP_LOGW(TAG, "Failed to allocate INTERNAL copy for forecast retry");
            }
        }
    }

    heap_caps_free(response_buffer);
    return success;
}

/* Single-pass optimized parser — with frequent yields and point storage in SPIRAM */
bool WeatherService::parse_forecast_response(const char* json, size_t len,
                                           weather_forecast_day_t* forecast,
                                           uint8_t* count,
                                           weather_today_outlook_t* today_outlook) {
    if (!count) {
        ESP_LOGE(TAG, "Invalid forecast count output");
        return false;
    }

    *count = 0;
    clear_today_outlook(today_outlook);
    if (!json || len == 0 || !forecast) {
        ESP_LOGE(TAG, "Invalid forecast JSON input");
        return false;
    }

    if (parse_forecast_with_static_tokens(json, len, forecast, count, today_outlook)) {
        ESP_LOGI(TAG, "Successfully parsed %d forecast days with static-token parser", *count);
        return true;
    }

    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
    if (!root) {
        size_t error_offset = 0;
        if (parse_end && parse_end >= json && parse_end <= json + len) {
            error_offset = static_cast<size_t>(parse_end - json);
        }
        ESP_LOGE(TAG, "Failed to parse forecast JSON with cJSON (len=%u, error_offset=%u)",
                 (unsigned)len, (unsigned)error_offset);
        ESP_LOGW(TAG, "Forecast response prefix: %.*s", (int)((len > 120) ? 120 : len), json);
        return false;
    }

    bool success = false;
    ForecastPoint* points = nullptr;

    do {
        cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "list");
        if (!cJSON_IsArray(list)) {
            cJSON* cod = cJSON_GetObjectItemCaseSensitive(root, "cod");
            cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
            ESP_LOGE(TAG, "No list array in forecast response (cod=%s, message=%s)",
                     cJSON_IsString(cod) && cod->valuestring ? cod->valuestring : "-",
                     cJSON_IsString(message) && message->valuestring ? message->valuestring : "-");
            break;
        }

        time_t now;
        time(&now);
        struct tm today_tm;
        localtime_r(&now, &today_tm);
        today_tm.tm_hour = 0;
        today_tm.tm_min = 0;
        today_tm.tm_sec = 0;
        time_t today_midnight = mktime(&today_tm);

        int array_size = cJSON_GetArraySize(list);
        if (array_size <= 0) {
            ESP_LOGW(TAG, "Forecast list array is empty");
            break;
        }
        ESP_LOGI(TAG, "Forecast list array size: %d", array_size);

        int max_points = (array_size < 40) ? array_size : 40;
        bool points_in_spiram = true;
        points = allocate_forecast_points((size_t)max_points, &points_in_spiram);
        if (!points) {
            ESP_LOGE(TAG, "Failed to allocate points array");
            break;
        }
        ESP_LOGD(TAG, "Allocated %s RAM for forecast points (%u items)",
                 points_in_spiram ? "SPIRAM" : "INTERNAL", (unsigned)max_points);
        memset(points, 0, sizeof(ForecastPoint) * (size_t)max_points);

        int point_count = 0;
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, list) {
            if (point_count >= max_points) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1));

            if (!cJSON_IsObject(entry)) {
                continue;
            }

            int64_t dt = 0;
            if (!json_get_number_i64(entry, "dt", &dt)) {
                continue;
            }

            ForecastPoint* point = &points[point_count];
            point->timestamp = static_cast<time_t>(dt);

            cJSON* main = cJSON_GetObjectItemCaseSensitive(entry, "main");
            if (cJSON_IsObject(main)) {
                json_get_number(main, "temp", &point->temp);
                json_get_number(main, "feels_like", &point->feels_like);
                json_get_number(main, "humidity", &point->humidity);
                json_get_number(main, "pressure", &point->pressure);
            }

            json_get_number(entry, "pop", &point->pop);

            cJSON* rain = cJSON_GetObjectItemCaseSensitive(entry, "rain");
            if (cJSON_IsObject(rain)) {
                json_get_number(rain, "3h", &point->rain);
            }

            cJSON* snow = cJSON_GetObjectItemCaseSensitive(entry, "snow");
            if (cJSON_IsObject(snow)) {
                json_get_number(snow, "3h", &point->snow);
            }

            cJSON* weather = cJSON_GetObjectItemCaseSensitive(entry, "weather");
            if (cJSON_IsArray(weather)) {
                cJSON* first_weather = cJSON_GetArrayItem(weather, 0);
                if (cJSON_IsObject(first_weather)) {
                    json_copy_string(first_weather, "description", point->description, sizeof(point->description));
                    json_copy_string(first_weather, "icon", point->icon, sizeof(point->icon));
                    if (point->description[0]) {
                        point->description[0] = (char)toupper((unsigned char)point->description[0]);
                    }
                }
            }

            cJSON* wind = cJSON_GetObjectItemCaseSensitive(entry, "wind");
            if (cJSON_IsObject(wind)) {
                json_get_number(wind, "speed", &point->wind_speed);
                json_get_number(wind, "deg", &point->wind_direction);
            }

            struct tm point_tm;
            localtime_r(&point->timestamp, &point_tm);
            int hour = point_tm.tm_hour;
            point->is_night = (hour >= 21 || hour <= 6);
            point->is_day = (hour >= 11 && hour <= 14);

            ++point_count;
        }

        populate_today_outlook_from_points(points, point_count, now, today_outlook);
        *count = aggregate_forecast_points(points, point_count, today_midnight, forecast);
        success = (*count > 0);
    } while (0);

    if (points) {
        heap_caps_free(points);
    }
    cJSON_Delete(root);

    if (!success) {
        ESP_LOGW(TAG, "No forecast data parsed");
    } else {
        ESP_LOGI(TAG, "Successfully parsed %d forecast days (starting from tomorrow)", *count);
    }

    return success;
}

/* ----------------- parse current weather (kept original but with minimal logging) ----------------- */

bool WeatherService::parse_weather_response(const char* json, size_t len, weather_data_t* data) {
    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, json, len) != OS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to start JSON parsing");
        return false;
    }

    bool success = false;

    do {
        if (json_obj_get_object(&jctx, "main") != OS_SUCCESS) {
            ESP_LOGE(TAG, "Failed to get 'main' object");
            break;
        }

        json_obj_get_float(&jctx, "temp", &data->temperature);
        json_obj_get_float(&jctx, "feels_like", &data->feels_like);
        json_obj_get_float(&jctx, "humidity", &data->humidity);
        json_obj_get_float(&jctx, "pressure", &data->pressure);
        json_obj_leave_object(&jctx);

        if (json_obj_get_object(&jctx, "wind") == OS_SUCCESS) {
            json_obj_get_float(&jctx, "speed", &data->wind_speed);
            json_obj_get_float(&jctx, "deg", &data->wind_direction);
            json_obj_leave_object(&jctx);
        }

        int weather_count = 0;
        if (json_obj_get_array(&jctx, "weather", &weather_count) == OS_SUCCESS && weather_count > 0) {
            if (json_arr_get_object(&jctx, 0) == OS_SUCCESS) {
                json_obj_get_string(&jctx, "description", data->description, sizeof(data->description) - 1);
                json_obj_get_string(&jctx, "icon", data->icon, sizeof(data->icon) - 1);
                json_arr_leave_object(&jctx);
            }
            json_obj_leave_array(&jctx);
        }

        if (json_obj_get_object(&jctx, "sys") == OS_SUCCESS) {
            json_obj_get_string(&jctx, "country", data->country, sizeof(data->country) - 1);

            int64_t sunrise, sunset;
            if (json_obj_get_int64(&jctx, "sunrise", &sunrise) == OS_SUCCESS) {
                data->sunrise = (uint32_t)sunrise;
            }
            if (json_obj_get_int64(&jctx, "sunset", &sunset) == OS_SUCCESS) {
                data->sunset = (uint32_t)sunset;
            }

            json_obj_leave_object(&jctx);
        }

        json_obj_get_string(&jctx, "name", data->city, sizeof(data->city) - 1);

        int64_t timestamp;
        if (json_obj_get_int64(&jctx, "dt", &timestamp) == OS_SUCCESS) {
            data->timestamp = (uint32_t)timestamp;
        }

        float pop = 0;
        if (json_obj_get_float(&jctx, "pop", &pop) == OS_SUCCESS) {
            data->pop = pop;
        }

        if (json_obj_get_object(&jctx, "rain") == OS_SUCCESS) {
            float rain_1h = 0;
            if (json_obj_get_float(&jctx, "1h", &rain_1h) == OS_SUCCESS) {
                data->rain = rain_1h;
            }
            json_obj_leave_object(&jctx);
        }

        if (json_obj_get_object(&jctx, "snow") == OS_SUCCESS) {
            float snow_1h = 0;
            if (json_obj_get_float(&jctx, "1h", &snow_1h) == OS_SUCCESS) {
                data->snow = snow_1h;
            }
            json_obj_leave_object(&jctx);
        }

        success = true;
        ESP_LOGI(TAG, "Successfully parsed weather data: %s, %.1f°C", data->city, data->temperature);

    } while (0);

    json_parse_end(&jctx);
    return success;
}

/* ----------------- URL encode helper ----------------- */

void WeatherService::url_encode(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) return;

    const char* hex = "0123456789ABCDEF";
    char* ptr = output;
    size_t remaining = output_size; // includes null

    while (*input && remaining > 1) {
        unsigned char c = (unsigned char)*input++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *ptr++ = (char)c;
            remaining -= 1;
        } else if (c == ' ') {
            if (remaining <= 3) break;
            *ptr++ = '%'; *ptr++ = '2'; *ptr++ = '0';
            remaining -= 3;
        } else {
            if (remaining <= 3) break;
            *ptr++ = '%';
            *ptr++ = hex[(c >> 4) & 0x0F];
            *ptr++ = hex[c & 0x0F];
            remaining -= 3;
        }
    }
    *ptr = '\0';
}
