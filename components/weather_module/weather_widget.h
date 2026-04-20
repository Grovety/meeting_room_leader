#pragma once
#include "lvgl.h"
#include "weather_module.h" /* weather_data_t, forecast types */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct weather_widget_t weather_widget_t;

weather_widget_t* weather_widget_create(lv_obj_t* parent);
void weather_widget_update(weather_widget_t* widget, const weather_data_t* data);
void weather_widget_refresh_language(weather_widget_t* widget);
void weather_widget_set_pos(weather_widget_t* widget, int32_t x, int32_t y);
void weather_widget_set_size(weather_widget_t* widget, int32_t width, int32_t height);
void weather_widget_set_align(weather_widget_t* widget, lv_align_t align, int32_t x_ofs, int32_t y_ofs);
lv_obj_t* weather_widget_get_obj(weather_widget_t* widget);
void weather_widget_set_click_callback(weather_widget_t* widget, void (*callback)(void*), void* user_data);
void weather_widget_delete(weather_widget_t* widget);

#ifdef __cplusplus
}
#endif
