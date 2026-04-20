// location_service.cpp
#include "location_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <json_parser.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wifi_manager.h" /* used to check Wi-Fi state */

static const char* TAG = "location_service";

// ============ Location Data ============
static double cached_lat        = 0.0;
static double cached_lon        = 0.0;
static char cached_city[150]    = "";
static char cached_timezone[64] = "";
static char location_buffer[2048];
static bool cached_valid                = false; // flag indicating cache was actually received from API
static SemaphoreHandle_t s_lookup_mutex = NULL;
static SemaphoreHandle_t s_state_mutex  = NULL;

static void ensure_state_mutex(void)
{
    if (! s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (! s_state_mutex) {
            ESP_LOGW(TAG, "Location: failed to create state mutex");
        }
    }
}

static bool lock_state(TickType_t timeout_ticks)
{
    ensure_state_mutex();
    if (! s_state_mutex) {
        return false;
    }

    return xSemaphoreTake(s_state_mutex, timeout_ticks) == pdTRUE;
}

static void unlock_state(void)
{
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }
}

static bool cached_state_is_valid(void)
{
    bool valid = false;
    if (lock_state(portMAX_DELAY)) {
        valid = cached_valid;
        unlock_state();
    }
    return valid;
}

static bool try_lock_lookup(void)
{
    if (! s_lookup_mutex) {
        s_lookup_mutex = xSemaphoreCreateMutex();
        if (! s_lookup_mutex) {
            ESP_LOGW(TAG, "Location: failed to create lookup mutex");
            return false;
        }
    }

    return xSemaphoreTake(s_lookup_mutex, 0) == pdTRUE;
}

static void unlock_lookup(void)
{
    if (s_lookup_mutex) {
        xSemaphoreGive(s_lookup_mutex);
    }
}

// ============ Helper Functions ============
static void fix_city_name(char* city, size_t city_size)
{
    if (! city || city_size == 0)
        return;

    city[city_size - 1] = '\0';
    size_t len          = strnlen(city, city_size);
    if (len == 0)
        return;

    // Convert first letter to uppercase, others to lowercase
    city[0] = toupper((unsigned char)city[0]);
    for (size_t i = 1; i < len; ++i) {
        city[i] = tolower((unsigned char)city[i]);
    }

    // Specific fix: "St " -> "Saint "
    if (len >= 3 && (strncmp(city, "St ", 3) == 0 || strncmp(city, "st ", 3) == 0)) {
        char temp_city[150] = {0};
        snprintf(temp_city, sizeof(temp_city) - 1, "Saint %s", city + 3);
        strncpy(city, temp_city, city_size - 1);
        city[city_size - 1] = '\0';
    }
}

typedef struct {
    size_t total_len;
    char* buf;
    size_t buf_size;
} loc_http_ctx_t;

static esp_err_t location_http_event_handler(esp_http_client_event_t* evt)
{
    loc_http_ctx_t* ctx = (loc_http_ctx_t*)evt->user_data;
    if (! ctx)
        return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            if (ctx->total_len + evt->data_len < ctx->buf_size) {
                memcpy(ctx->buf + ctx->total_len, evt->data, evt->data_len);
                ctx->total_len += evt->data_len;
                ctx->buf[ctx->total_len] = '\0';
            } else {
                ESP_LOGW(TAG, "Location buffer overflow, truncating");
                /* On overflow, clear accumulated data (not ideal, but safe) */
                ctx->total_len = 0;
                ctx->buf[0]    = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "Location: Received %u bytes", (unsigned)ctx->total_len);

        if (ctx->total_len == 0) {
            ESP_LOGW(TAG, "Location: empty response");
            ctx->total_len = 0;
            break;
        }

        jparse_ctx_t jctx;
        if (json_parse_start(&jctx, ctx->buf, ctx->total_len) == OS_SUCCESS) {
            char status[16] = {0};
            if (json_obj_get_string(&jctx, "status", status, sizeof(status)) == OS_SUCCESS &&
                strcmp(status, "success") == 0) {

                char lat_str[32] = {0}, lon_str[32] = {0}, city_str[150] = {0}, region_str[100] = {0}, tz_str[64] = {0};
                bool have_coords  = false;
                bool have_city    = false;
                bool have_tz      = false;
                double parsed_lat = 0.0;
                double parsed_lon = 0.0;

                // IMPORTANT: do not overwrite previous coordinates with defaults; assign only when values are present
                if (json_obj_get_string(&jctx, "lat", lat_str, sizeof(lat_str)) == OS_SUCCESS &&
                    json_obj_get_string(&jctx, "lon", lon_str, sizeof(lon_str)) == OS_SUCCESS) {
                    double lat = atof(lat_str);
                    double lon = atof(lon_str);
                    // consider coordinates valid if they are not 0/0
                    if (! (lat == 0.0 && lon == 0.0)) {
                        parsed_lat  = lat;
                        parsed_lon  = lon;
                        have_coords = true;
                        ESP_LOGI(TAG, "Location: Got coordinates - lat: %f, lon: %f", parsed_lat, parsed_lon);
                    } else {
                        ESP_LOGW(TAG, "Location: coordinates are 0, ignoring");
                    }
                }

                if (json_obj_get_string(&jctx, "city", city_str, sizeof(city_str)) == OS_SUCCESS) {
                    if (strlen(city_str) > 0) {
                        fix_city_name(city_str, sizeof(city_str));
                        have_city = true;
                    }
                }

                if (! have_city) {
                    if (json_obj_get_string(&jctx, "regionName", region_str, sizeof(region_str)) == OS_SUCCESS) {
                        if (strlen(region_str) > 0) {
                            fix_city_name(region_str, sizeof(region_str));
                            strncpy(city_str, region_str, sizeof(city_str) - 1);
                            city_str[sizeof(city_str) - 1] = '\0';
                            have_city                      = true;
                        }
                    }
                }

                if (json_obj_get_string(&jctx, "timezone", tz_str, sizeof(tz_str)) == OS_SUCCESS) {
                    if (strlen(tz_str) > 0) {
                        have_tz = true;
                        ESP_LOGI(TAG, "Location: Timezone - %s", tz_str);
                    }
                }

                // Treat as success if at least one useful field is received (coords OR city OR tz)
                if (have_coords || have_city || have_tz) {
                    double log_lat                       = 0.0;
                    double log_lon                       = 0.0;
                    char log_city[sizeof(cached_city)]   = {0};
                    char log_tz[sizeof(cached_timezone)] = {0};

                    if (lock_state(portMAX_DELAY)) {
                        if (have_coords) {
                            cached_lat = parsed_lat;
                            cached_lon = parsed_lon;
                        }
                        if (have_city) {
                            strncpy(cached_city, city_str, sizeof(cached_city) - 1);
                            cached_city[sizeof(cached_city) - 1] = '\0';
                        }
                        if (have_tz) {
                            strncpy(cached_timezone, tz_str, sizeof(cached_timezone) - 1);
                            cached_timezone[sizeof(cached_timezone) - 1] = '\0';
                        }
                        cached_valid = true;

                        log_lat = cached_lat;
                        log_lon = cached_lon;
                        strncpy(log_city, cached_city, sizeof(log_city) - 1);
                        log_city[sizeof(log_city) - 1] = '\0';
                        strncpy(log_tz, cached_timezone, sizeof(log_tz) - 1);
                        log_tz[sizeof(log_tz) - 1] = '\0';
                        unlock_state();
                    }

                    ESP_LOGI(TAG, "Location: cache updated: city='%s' tz='%s' (coords: %f,%f)", log_city, log_tz,
                             log_lat, log_lon);
                } else {
                    ESP_LOGW(TAG, "Location: API returned success but no useful fields");
                }
            } else {
                ESP_LOGE(TAG, "Location: API returned failure status or missing 'status'");
            }
            json_parse_end(&jctx);
        } else {
            ESP_LOGE(TAG, "Location: Failed to parse JSON response");
        }

        ctx->total_len = 0;
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "Location: HTTP error");
        ctx->total_len = 0;
        break;

    default:
        break;
    }
    return ESP_OK;
}

// ============ Timezone mapping ============

const char* location_service_convert_iana_to_posix(const char* iana_tz)
{
    if (! iana_tz || strlen(iana_tz) == 0) {
        ESP_LOGI(TAG, "Empty timezone, using GMT0");
        return "GMT0";
    }

    ESP_LOGI(TAG, "Converting IANA timezone: %s", iana_tz);

    // Europe (partial list; extend if needed)
    if (strcmp(iana_tz, "Europe/Moscow") == 0)
        return "MSK-3";
    else if (strcmp(iana_tz, "Europe/Kaliningrad") == 0)
        return "EET-2";
    else if (strcmp(iana_tz, "Europe/Volgograd") == 0)
        return "MSK-3";
    else if (strcmp(iana_tz, "Europe/Samara") == 0)
        return "SAMT-4";
    else if (strcmp(iana_tz, "Europe/Yekaterinburg") == 0)
        return "YEKT-5";
    else if (strcmp(iana_tz, "Europe/Omsk") == 0)
        return "OMST-6";
    else if (strcmp(iana_tz, "Europe/Krasnoyarsk") == 0)
        return "KRAT-7";
    else if (strcmp(iana_tz, "Europe/Irkutsk") == 0)
        return "IRKT-8";
    else if (strcmp(iana_tz, "Europe/London") == 0)
        return "GMT0BST";
    else if (strcmp(iana_tz, "Europe/Paris") == 0) {
        return "CET-1CEST,M3.5.0,M10.5.0/3";
    }

    // Asia
    else if (strcmp(iana_tz, "Asia/Yakutsk") == 0)
        return "YAKT-9";
    else if (strcmp(iana_tz, "Asia/Vladivostok") == 0)
        return "VLAT-10";
    else if (strcmp(iana_tz, "Asia/Magadan") == 0)
        return "MAGT-11";
    else if (strcmp(iana_tz, "Asia/Kamchatka") == 0)
        return "PETT-12";
    else if (strcmp(iana_tz, "Asia/Novosibirsk") == 0)
        return "NOVT-6";
    else if (strcmp(iana_tz, "Asia/Novokuznetsk") == 0)
        return "KRAT-7";
    else if (strcmp(iana_tz, "Asia/Almaty") == 0)
        return "ALMT-6";
    else if (strcmp(iana_tz, "Asia/Tokyo") == 0)
        return "JST-9";
    else if (strcmp(iana_tz, "Asia/Shanghai") == 0 || strcmp(iana_tz, "Asia/Beijing") == 0)
        return "CST-8";
    else if (strcmp(iana_tz, "Asia/Seoul") == 0)
        return "KST-9";
    else if (strcmp(iana_tz, "Asia/Singapore") == 0)
        return "SGT-8";

    // Americas
    else if (strcmp(iana_tz, "America/New_York") == 0)
        return "EST5EDT";
    else if (strcmp(iana_tz, "America/Chicago") == 0)
        return "CST6CDT";
    else if (strcmp(iana_tz, "America/Denver") == 0)
        return "MST7MDT";
    else if (strcmp(iana_tz, "America/Los_Angeles") == 0)
        return "PST8PDT";

    // Australia
    else if (strcmp(iana_tz, "Australia/Sydney") == 0)
        return "AEST-10AEDT";
    else if (strcmp(iana_tz, "Australia/Melbourne") == 0)
        return "AEST-10AEDT";
    else if (strcmp(iana_tz, "Australia/Perth") == 0)
        return "AWST-8";

    // Common replacements
    else if (strstr(iana_tz, "Paris") != NULL)
        return "CET-1CEST";
    else if (strstr(iana_tz, "Europe") != NULL)
        return "CET-1CEST";

    ESP_LOGW(TAG, "Unknown timezone: %s, using GMT0", iana_tz);
    return "GMT0";
}

const char* location_service_detect_timezone_by_city(const char* city)
{
    if (! city || strlen(city) == 0) {
        return "GMT0";
    }
    ESP_LOGI(TAG, "Detecting timezone for city: %s", city);

    // Create a local lowercase copy for case-insensitive matching
    char lower_city[160] = {0};
    size_t clen          = strlen(city);
    size_t copy_len      = (clen < sizeof(lower_city) - 1) ? clen : (sizeof(lower_city) - 1);
    for (size_t i = 0; i < copy_len; ++i) {
        lower_city[i] = (char)tolower((unsigned char)city[i]);
    }
    lower_city[copy_len] = '\0';

    // Russia / nearby regions
    if (strstr(lower_city, "moscow") != NULL || strstr(lower_city, "moskva") != NULL)
        return "MSK-3";
    if (strstr(lower_city, "saint") != NULL || strstr(lower_city, "petersburg") != NULL)
        return "MSK-3";
    if (strstr(lower_city, "kaliningrad") != NULL)
        return "EET-2";
    if (strstr(lower_city, "samara") != NULL)
        return "SAMT-4";
    if (strstr(lower_city, "yekaterinburg") != NULL || strstr(lower_city, "yekater") != NULL)
        return "YEKT-5";
    if (strstr(lower_city, "novosibir") != NULL)
        return "NOVT-6";
    if (strstr(lower_city, "krasnoyarsk") != NULL)
        return "KRAT-7";
    if (strstr(lower_city, "irkutsk") != NULL)
        return "IRKT-8";

    // United Kingdom
    if (strstr(lower_city, "london") != NULL)
        return "GMT0BST";

    // France and major French cities -> CET/CEST
    const char* french_cities[] = {"paris",
                                   "lyon",
                                   "marseille",
                                   "toulouse",
                                   "nice",
                                   "nantes",
                                   "strasbourg",
                                   "montpellier",
                                   "bordeaux",
                                   "lille",
                                   "rennes",
                                   "reims",
                                   "le havre",
                                   "saint-etienne",
                                   "toulon",
                                   "grenoble",
                                   "dijon",
                                   "angers",
                                   "nimes",
                                   "villeurbanne",
                                   "clermont-ferrand",
                                   "le mans",
                                   "aix-en-provence",
                                   "brest",
                                   "limoges",
                                   "bayonne",
                                   "caen",
                                   "amiens",
                                   "metz",
                                   "perpignan",
                                   "besancon",
                                   "orleans",
                                   "rouen",
                                   "mulhouse",
                                   "nancy",
                                   "tours",
                                   "avignon",
                                   "colmar",
                                   "saint-malo",
                                   "rodez",
                                   "bergerac"};
    for (size_t i = 0; i < sizeof(french_cities) / sizeof(french_cities[0]); ++i) {
        if (strstr(lower_city, french_cities[i]) != NULL)
            return "CET-1CEST";
    }

    // Other major European cities (CET)
    if (strstr(lower_city, "berlin") != NULL)
        return "CET-1CEST";
    if (strstr(lower_city, "rome") != NULL)
        return "CET-1CEST";
    if (strstr(lower_city, "madrid") != NULL)
        return "CET-1CEST";

    // Fallbacks and other regions
    if (strstr(lower_city, "france") != NULL)
        return "CET-1CEST";
    if (strstr(lower_city, "kiev") != NULL || strstr(lower_city, "kyiv") != NULL)
        return "EET-2EEST";
    if (strstr(lower_city, "minsk") != NULL)
        return "MSK-3";
    if (strstr(lower_city, "tokyo") != NULL)
        return "JST-9";
    if (strstr(lower_city, "new york") != NULL || strstr(lower_city, "nyc") != NULL)
        return "EST5EDT";

    return "GMT0";
}

// ============ Public APIs ============
void location_service_init(void)
{
    // Do not mark cache as valid by default; fallback is applied in main if IP lookup fails.
    if (lock_state(portMAX_DELAY)) {
        cached_valid       = false;
        cached_lat         = 0.0;
        cached_lon         = 0.0;
        cached_city[0]     = '\0';
        cached_timezone[0] = '\0';
        unlock_state();
    }
    ESP_LOGI(TAG, "Location service initialized (no cached location)");
}

bool location_service_get(double* lat, double* lon, char* city, size_t city_len)
{
    if (! lock_state(portMAX_DELAY)) {
        return false;
    }

    if (! cached_valid) {
        unlock_state();
        return false;
    }

    if (lat)
        *lat = cached_lat;
    if (lon)
        *lon = cached_lon;
    if (city && city_len > 0) {
        strncpy(city, cached_city, city_len - 1);
        city[city_len - 1] = '\0';
    }
    unlock_state();
    return true;
}

bool location_service_get_timezone(char* timezone, size_t timezone_len)
{
    if (! lock_state(portMAX_DELAY)) {
        return false;
    }

    bool ok = false;
    if (cached_valid && timezone && timezone_len > 0 && strlen(cached_timezone) > 0) {
        strncpy(timezone, cached_timezone, timezone_len - 1);
        timezone[timezone_len - 1] = '\0';
        ok                         = true;
    }
    unlock_state();
    return ok;
}

bool location_service_get_by_ip(void)
{
    ESP_LOGI(TAG, "Location: Getting from IP...");

    /* If Wi-Fi is not connected yet, do not perform a blocking request.
       Return success only if a valid cache exists (old data),
       otherwise return false to trigger fallback. */
    if (! wifi_manager_is_connected()) {
        if (cached_state_is_valid()) {
            ESP_LOGI(TAG, "Location: Wi-Fi not connected, returning cached data");
            return true;
        } else {
            ESP_LOGW(TAG, "Location: Wi-Fi not connected, skipping IP lookup");
            return false;
        }
    }

    if (! try_lock_lookup()) {
        if (cached_state_is_valid()) {
            ESP_LOGI(TAG, "Location: lookup already in progress, returning cached data");
            return true;
        }
        ESP_LOGW(TAG, "Location: lookup already in progress, skipping duplicate request");
        return false;
    }

    // Reset buffer and context
    memset(location_buffer, 0, sizeof(location_buffer));
    loc_http_ctx_t ctx;
    ctx.total_len = 0;
    ctx.buf       = location_buffer;
    ctx.buf_size  = sizeof(location_buffer);

    esp_http_client_config_t config = {};
    config.url                      = "http://ip-api.com/"
                 "json?fields=status,message,country,countryCode,region,regionName,city,lat,lon,timezone,query";
    config.event_handler         = location_http_event_handler;
    config.user_data             = &ctx;
    config.timeout_ms            = 15000;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 10;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (! client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        unlock_lookup();
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-Location-Service/1.0");

    esp_err_t err = esp_http_client_perform(client);

    bool success = false;
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "Location: HTTP 200 OK");
            // cached_valid is set by handler only if parsing produced data
            if (cached_state_is_valid()) {
                success = true;
            } else {
                ESP_LOGW(TAG, "Location: HTTP 200 but parsing didn't set cached_valid");
            }
        } else {
            ESP_LOGE(TAG, "Location: HTTP status: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Location: HTTP error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    unlock_lookup();
    return success;
}

void location_service_set_manual(double lat, double lon, const char* city)
{
    if (! lock_state(portMAX_DELAY)) {
        return;
    }

    cached_lat = lat;
    cached_lon = lon;
    if (city) {
        strncpy(cached_city, city, sizeof(cached_city) - 1);
        cached_city[sizeof(cached_city) - 1] = '\0';
        if (strlen(cached_city) > 0) {
            cached_city[0] = toupper((unsigned char)cached_city[0]);
            for (int i = 1; cached_city[i] != '\0'; ++i) {
                cached_city[i] = tolower((unsigned char)cached_city[i]);
            }
        }
    } else {
        cached_city[0] = '\0';
    }
    cached_valid = true;
    unlock_state();

    ESP_LOGI(TAG, "Location: Set manually - %s (%f, %f)", city ? city : "", lat, lon);
}

bool location_service_is_wifi_connected(void) { return wifi_manager_is_connected(); }

void location_service_wait_for_wifi(void)
{
    while (! location_service_is_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

uint32_t location_service_get_time(void)
{
    time_t now;
    time(&now);
    return (uint32_t)now;
}

bool location_service_is_time_synced(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    return (timeinfo.tm_year > (2020 - 1900));
}
