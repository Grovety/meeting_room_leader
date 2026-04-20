// weather_integration.h
#pragma once

#include "weather_module.h"
#include "lvgl.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void weather_integration_init(void);
void weather_integration_set_location(double lat, double lon, const char* city);
void weather_integration_set_api_key(const char* api_key);
void weather_integration_get_api_key_copy(char* api_key, size_t api_key_size);
void weather_integration_set_metric_units(bool metric_units);
bool weather_integration_get_metric_units(void);
void weather_integration_start(void);
void weather_integration_stop(void);
bool weather_integration_is_running(void);

bool weather_integration_get_weather(weather_data_t* data);
bool weather_integration_get_forecast(weather_forecast_day_t* forecast, uint8_t* count);

void weather_integration_show_fullscreen(void);
lv_obj_t* weather_integration_create_widget(lv_obj_t* parent);
void weather_integration_refresh_language(void);
void weather_integration_destroy_widget(void);

void weather_integration_widget_set_pos(lv_obj_t* widget, int32_t x, int32_t y);
void weather_integration_widget_set_size(lv_obj_t* widget, int32_t width, int32_t height);

#ifdef __cplusplus
}
#endif
