#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timestamp;           // Day timestamp
    float temp_day;               // Day temperature
    float temp_night;             // Night temperature
    float feels_like_day;         // Feels-like temperature (day)
    float humidity;               // Humidity
    float pressure;               // Pressure
    float wind_speed;             // Wind speed
    float wind_direction;         // Wind direction
    char description[64];         // Weather description
    char icon[8];                 // Icon
    float pop;                    // Precipitation probability (0-1)
    float rain;                   // Rain amount (mm)
    float snow;                   // Snow amount (mm)
    
    uint32_t next_precip_time;    // Time of nearest precipitation (timestamp)
    char next_precip_type[16];    // Type: "rain", "snow", "mixed"
    float next_precip_pop;        // Probability in the near term
} weather_forecast_day_t;

typedef struct {
    bool valid;                   // Whether outlook is computed for the rest of today
    float pop;                    // Max precipitation probability for today (0-1)
    float rain;                   // Total rain for today (mm)
    float snow;                   // Total snow for today (mm)
    uint32_t precipitation_time;  // Time of nearest precipitation today
    char precipitation_type[16];  // Type: "rain", "snow", "mixed"
} weather_today_outlook_t;

typedef struct {
    char api_key[64];
    double latitude;
    double longitude;
    char city[64];
    bool use_coordinates; 
    bool metric_units;
    int update_interval_sec; 
    char wifi_ssid[32];
    char wifi_password[64];
    bool use_ip_location;
} weather_config_t;

typedef struct {
    float temperature;
    float feels_like;
    float humidity;
    float pressure;
    float wind_speed;
    float wind_direction;
    char description[64];
    char icon[8];
    char city[64];
    char country[16];
    uint32_t timestamp;
    uint32_t sunrise;
    uint32_t sunset;
    float pop;
    float rain;
    float snow;
    uint32_t precipitation_time;
} weather_data_t;

typedef enum {
    WEATHER_DATA_STATUS_OK = 0,
    WEATHER_DATA_STATUS_API_KEY_REQUIRED,
    WEATHER_DATA_STATUS_INVALID_API_KEY,
    WEATHER_DATA_STATUS_UNAVAILABLE,
} weather_data_status_t;

void weather_module_init(void);
void weather_module_set_config(const weather_config_t* config);
void weather_module_start(void);
void weather_module_stop(void);
bool weather_module_is_running(void);
void weather_module_request_update(void);
void weather_module_convert_cached_units(bool to_metric);
bool weather_get_current_data(weather_data_t* data);
bool weather_get_forecast_data(weather_forecast_day_t* forecast, uint8_t* count);
bool weather_get_today_outlook(weather_today_outlook_t* outlook);
weather_data_status_t weather_module_get_status(void);
void weather_module_set_update_callback(void (*callback)(const weather_data_t*, const weather_forecast_day_t*, uint8_t));

#ifdef __cplusplus
}
#endif
