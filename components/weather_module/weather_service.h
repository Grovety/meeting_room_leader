// weather_service.h - with forecast support
#pragma once

#include "weather_module.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> 

#ifdef __cplusplus
extern "C" {
#endif

class WeatherService {
private:
    struct Ctx {
        char api_key[64];
        char latitude[20];
        char longitude[20];
        char location_name[150];
        bool use_coordinates;
        bool metric_units;
    } *ctx_;

public:
    WeatherService();
    ~WeatherService();
    
    bool init(const weather_config_t* config);
    bool fetch_current_weather(weather_data_t* data);
    bool fetch_forecast(weather_forecast_day_t* forecast, uint8_t* count,
                        weather_today_outlook_t* today_outlook = nullptr);
    
private:
    bool parse_weather_response(const char* json, size_t len, weather_data_t* data);
    bool parse_forecast_response(const char* json, size_t len, weather_forecast_day_t* forecast, uint8_t* count,
                                 weather_today_outlook_t* today_outlook);
    void url_encode(const char* input, char* output, size_t output_size);
};

int weather_service_get_last_http_status(void);

#ifdef __cplusplus
}
#endif
