#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void on_btn_say_pressed(lv_event_t * e);

void ui_notify_tts_finished(void);

void on_wifi_update_from_uart(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
