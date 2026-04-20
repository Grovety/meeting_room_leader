// ui_WeatherScreen.c
#include "ui.h"
#include "lvgl.h"
#include "weather_integration.h"

lv_obj_t *ui_WeatherScreen = NULL;

void ui_WeatherScreen_screen_init(void)
{
    // Do not create a local screen manually; use weather_integration,
    // which safely and correctly initializes/shows fullscreen UI.
    // This removes duplicated screens and possible dangling pointers.
    weather_integration_show_fullscreen();
}
