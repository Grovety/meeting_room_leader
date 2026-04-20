#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "weather_module.h"

#ifdef __cplusplus
extern "C" {
#endif

bool weather_ui_attach(lv_obj_t* screen);

void weather_ui_update_current(const weather_data_t* data);
void weather_ui_update_forecast(const weather_forecast_day_t* forecast, uint8_t count);

#ifdef __cplusplus
}
#endif
