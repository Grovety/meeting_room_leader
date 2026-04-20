#include "ui.h"
#include "lvgl.h"

extern lv_obj_t *ui_WeatherScreen;   // <-- ADD

void ui_weather_go_back(lv_obj_t* old_screen)
{
    if (ui_Screen8 == NULL) {
        ui_Screen8_screen_init();
    }

    lv_scr_load(ui_Screen8);

    if (old_screen) {
        lv_obj_del_async(old_screen);
    }

    ui_WeatherScreen = NULL;         // now OK
}
