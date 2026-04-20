#include "ui.h"
#include "lvgl.h"
#include "device_name_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "weather_integration.h"

#include "language_manager.h" /* NEW: central language manager */
#include "wifi_manager.h" /* for wifi connection status */

bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock(void);

static bool screen8_dispatch_async(lv_async_cb_t cb, void *arg)
{
    if (!lvgl_port_lock(-1)) {
        return false;
    }

    lv_res_t rc = lv_async_call(cb, arg);
    lvgl_port_unlock();
    return rc == LV_RES_OK;
}
/* ===================== Forward declarations for local helpers ===================== */
static void ui_update_events_labels(void);

#define SCREEN8_OUTER_PAD_X 20
#define SCREEN8_OUTER_PAD_Y 10
#define SCREEN8_SECTION_GAP 10
#define SCREEN8_HEADER_H 46
#define SCREEN8_FOOTER_H 44
#define SCREEN8_MAIN_GAP 10
#define SCREEN8_LEFT_COL_W 352
#define SCREEN8_RIGHT_COL_W 398
#define SCREEN8_CARD_RADIUS 24
#define SCREEN8_LEFT_TEXT_W 316
#define SCREEN8_STATUS_TEXT_W 296
#define SCREEN8_COLOR_BG_TOP 0x12100F
#define SCREEN8_COLOR_BG_BOTTOM 0x12100F
#define SCREEN8_COLOR_SURFACE 0x2A2522
#define SCREEN8_COLOR_SURFACE_ALT 0x231E1B
#define SCREEN8_COLOR_SURFACE_RAISED 0x342D29
#define SCREEN8_COLOR_SURFACE_PRESSED 0x433A35
#define SCREEN8_COLOR_TEXT 0xF5EFE8
#define SCREEN8_COLOR_TEXT_MUTED 0xC2B5AA
#define SCREEN8_COLOR_TEXT_DIM 0x968A7F
#define SCREEN8_COLOR_SHADOW 0x000000
#define SCREEN8_COLOR_ACCENT 0x6E955F
#define SCREEN8_COLOR_ACCENT_LIGHT 0x93B783
#define SCREEN8_COLOR_ACCENT_SHADOW 0x4B6843
#define SCREEN8_COLOR_DANGER 0x7A5250
#define SCREEN8_COLOR_DANGER_LIGHT 0xA77271
#define SCREEN8_COLOR_DANGER_SHADOW 0x573937
#define SCREEN8_COLOR_WARNING 0xD29A57
#define SCREEN8_COLOR_BUTTON 0x37302C
#define SCREEN8_COLOR_BUTTON_PRESSED 0x49403A
/* Strong "occupied" palette for active booking timer state */
#define SCREEN8_COLOR_OCCUPIED_BG 0xD32F2F
#define SCREEN8_COLOR_OCCUPIED_BORDER 0xFF8A80
#define SCREEN8_COLOR_OCCUPIED_SHADOW 0x7F1D1D
/* ===================== UI object handles ===================== */

lv_obj_t *ui_Screen8 = NULL;

lv_obj_t *ui_Container2 = NULL;
lv_obj_t *ui_Label4 = NULL;
lv_obj_t *ui_Image4 = NULL;

/* NEW: language switch - single label approach */
lv_obj_t *ui_LangSwitch = NULL;
lv_obj_t *ui_LangLabel  = NULL;

/* language animation state */
static bool s_lang_anim_running = false;
/* fixed original Y of the small LangLabel (to avoid cumulative drift) */
static lv_coord_t s_lang_label_y_orig = LV_COORD_MIN;

lv_obj_t *ui_Container4 = NULL;
lv_obj_t *ui_Container8 = NULL;

lv_obj_t *ui_Container26 = NULL;
lv_obj_t *ui_Label41 = NULL;  // time
lv_obj_t *ui_Label42 = NULL;  // date
lv_obj_t *ui_ImgBtn = NULL;
lv_obj_t *ui_ImgBtnImg = NULL;

/* timer for updating wifi icon visibility */
static lv_timer_t *s_wifi_icon_timer = NULL;


lv_obj_t *ui_Container27 = NULL;

/* Status card */
static lv_obj_t *ui_Panel4 = NULL;      // status card
static lv_obj_t *ui_Label44 = NULL;     // "Tap to book" (hidden in booked)
static lv_obj_t *ui_TimerLabel = NULL;  // mm:ss or H:MM:SS (shown only in booked)

/* Weather container */
lv_obj_t *ui_Container25 = NULL;           // right block container
static lv_obj_t *s_weather_widget = NULL;  // root widget object (to reuse click logic)

/* Booking overlay + drawer */
static lv_obj_t *ui_BookingOverlay = NULL;
static lv_obj_t *ui_BookingDrawer  = NULL;
static lv_obj_t *ui_BookingCloseBtn = NULL;
static lv_obj_t *ui_BookingCloseLbl = NULL;

static lv_obj_t *ui_BookingHeader = NULL;
lv_obj_t *ui_Label5 = NULL;     // "Booking room"
lv_obj_t *ui_Label7 = NULL;     // "Time for booking:"
lv_obj_t *ui_Container28 = NULL;

lv_obj_t *ui_Button2 = NULL;  // 15 min
lv_obj_t *ui_Label47 = NULL;
lv_obj_t *ui_Button3 = NULL;  // 30 min
lv_obj_t *ui_Label48 = NULL;
lv_obj_t *ui_Button4 = NULL;  // 45 min
lv_obj_t *ui_Label49 = NULL;
lv_obj_t *ui_Button5 = NULL;  // 1 hour
lv_obj_t *ui_Label50 = NULL;
lv_obj_t *ui_WifiIcon = NULL;   // header wifi status icon (shown when connected)

/* NEW buttons */
lv_obj_t *ui_ButtonFullDay = NULL; // All day
lv_obj_t *ui_LabelFullDay = NULL;

lv_obj_t *ui_ButtonManual = NULL; // Manual picker
lv_obj_t *ui_LabelManual = NULL;

lv_obj_t *ui_Button6 = NULL;  // Start now
lv_obj_t *ui_Label51 = NULL;

/* Manual picker modal (centered) */
static lv_obj_t *ui_ManualPanel = NULL;
static lv_obj_t *ui_ManualInner = NULL;
static lv_obj_t *ui_RollerHours = NULL;
static lv_obj_t *ui_RollerMins  = NULL;
static lv_obj_t *ui_ManualConfirmBtn = NULL;
static lv_obj_t *ui_ManualCancelBtn  = NULL;

/* NEW: explicit label pointers for manual modal buttons (fix compile error) */
static lv_obj_t *ui_ManualCancelLbl = NULL;
static lv_obj_t *ui_ManualConfirmLbl = NULL;

/* NEW: separate selection frames for hours and minutes */
static lv_obj_t *ui_HoursSelFrame = NULL;
static lv_obj_t *ui_MinsSelFrame  = NULL;

/* bottom nav */
lv_obj_t *ui_Container3 = NULL;

lv_obj_t *ui_Panel2 = NULL;
lv_obj_t *ui_Image12 = NULL;
lv_obj_t *ui_Label38 = NULL;

lv_obj_t *ui_Panel1 = NULL;
lv_obj_t *ui_Image1 = NULL;
lv_obj_t *ui_Label6 = NULL;

/* bottom Weather panel */
lv_obj_t *ui_Panel20 = NULL;
lv_obj_t *ui_Image2 = NULL;
lv_obj_t *ui_Label45 = NULL;

/* ===================== Drawer animation state ===================== */

static bool s_booking_open = false;
static int  s_drawer_w = 520;
static int  s_closed_x = 800;
static int  s_open_x   = 280; // 800-520

#define BOOKING_SHOW_OVERLAY_ANIM_MS 55
#define BOOKING_SHOW_DRAWER_ANIM_MS 65
#define BOOKING_HIDE_OVERLAY_ANIM_MS 55
#define BOOKING_HIDE_DRAWER_ANIM_MS 65

static void anim_x_cb(void * var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v); }
static void anim_y_cb(void * var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v); }
static void anim_obj_opa_cb(void * var, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0); }
static void booking_set_visible(bool open, bool anim);

/* overlay fade */
static void anim_opa_cb(void * var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t*)var;
    lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0);
}

/* ===================== Screen8 clock task (like weather_ui.cpp) ===================== */

typedef struct {
    char time_str[8];   // "HH:MM"
    int year;
    int month;
    int day;
    int wday;
} screen8_clock_msg_t;

static TaskHandle_t s_screen8_clock_task = NULL;
static volatile bool s_screen8_clock_run = false;

static const char *screen8_day_name(int wday)
{
    static const char* days_en[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char* days_de[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    if (wday < 0 || wday > 6) return "---";
    return language_get_en() ? days_en[wday] : days_de[wday];
}

static const char *screen8_month_name(int month)
{
    static const char* months_en[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char* months_de[] = {"1月","2月","3月","4月","5月","6月","7月","8月","9月","10月","11月","12月"};
    if (month < 1 || month > 12) return "---";
    return language_get_en() ? months_en[month - 1] : months_de[month - 1];
}

static void screen8_format_date_text(int month, int day, int wday, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (month < 1 || month > 12 || day < 1 || wday < 0 || wday > 6) {
        snprintf(out, out_sz, "---");
        return;
    }

    snprintf(out, out_sz, "%s, %d %s", screen8_day_name(wday), day, screen8_month_name(month));
    out[out_sz - 1] = '\0';
}

static void screen8_get_display_size(int *w, int *h)
{
    lv_disp_t *disp = lv_disp_get_default();
    int width = 800;
    int height = 480;

    if (disp) {
        width = (int)lv_disp_get_hor_res(disp);
        height = (int)lv_disp_get_ver_res(disp);
    }

    if (w) *w = width;
    if (h) *h = height;
}

static void screen8_get_main_area(lv_coord_t *x, lv_coord_t *y, lv_coord_t *w, lv_coord_t *h)
{
    int screen_w = 800;
    int screen_h = 480;
    screen8_get_display_size(&screen_w, &screen_h);

    lv_coord_t main_x = SCREEN8_OUTER_PAD_X;
    lv_coord_t main_y = SCREEN8_OUTER_PAD_Y + SCREEN8_HEADER_H + SCREEN8_SECTION_GAP;
    lv_coord_t main_w = (lv_coord_t)(screen_w - (SCREEN8_OUTER_PAD_X * 2));
    lv_coord_t main_h = (lv_coord_t)(screen_h - (SCREEN8_OUTER_PAD_Y * 2) - SCREEN8_HEADER_H - SCREEN8_FOOTER_H -
                                     (SCREEN8_SECTION_GAP * 2));

    if (main_h < 320) {
        main_h = 320;
    }

    if (x) *x = main_x;
    if (y) *y = main_y;
    if (w) *w = main_w;
    if (h) *h = main_h;
}

static lv_coord_t screen8_get_left_card_height(void)
{
    lv_coord_t main_h = 0;
    screen8_get_main_area(NULL, NULL, NULL, &main_h);

    lv_coord_t card_h = (main_h - SCREEN8_MAIN_GAP) / 2;
    if (card_h < 100) {
        card_h = 100;
    }

    return card_h;
}

static void screen8_style_surface_card(lv_obj_t *obj, lv_color_t bg, lv_opa_t bg_opa, lv_color_t border, lv_coord_t radius)
{
    if (!obj) return;

    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, border, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(obj, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(obj, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(SCREEN8_COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_x(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void screen8_style_header_button(lv_obj_t *obj)
{
    screen8_style_surface_card(obj, lv_color_hex(SCREEN8_COLOR_BUTTON), 255, lv_color_hex(SCREEN8_COLOR_BUTTON), 14);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(SCREEN8_COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
}

static void screen8_style_nav_item(lv_obj_t *obj)
{
    screen8_style_surface_card(obj, lv_color_hex(SCREEN8_COLOR_BUTTON), 235, lv_color_hex(SCREEN8_COLOR_BUTTON), 16);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(SCREEN8_COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_PRESSED);
}

static void screen8_refresh_clock_now(void)
{
    time_t now;
    time(&now);
    if (now <= 1000000) return;

    struct tm ti;
    localtime_r(&now, &ti);

    char time_buf[8];
    char date_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    screen8_format_date_text(ti.tm_mon + 1, ti.tm_mday, ti.tm_wday, date_buf, sizeof(date_buf));

    if (ui_Label41) lv_label_set_text(ui_Label41, time_buf);
    if (ui_Label42) lv_label_set_text(ui_Label42, date_buf);
}

/* IMPORTANT: use lv_async_call with a valid callback, not NULL.
*/
static void screen8_clock_apply_cb(void *p)
{
    screen8_clock_msg_t *m = (screen8_clock_msg_t*)p;
    if (!m) return;

    /* If screen already destroyed -> pointers are NULL */
    if (ui_Label41) lv_label_set_text(ui_Label41, m->time_str);
    if (ui_Label42) {
        char date_buf[32];
        screen8_format_date_text(m->month, m->day, m->wday, date_buf, sizeof(date_buf));
        lv_label_set_text(ui_Label42, date_buf);
    }

    free(m);
}

static void screen8_clock_task(void *arg)
{
    (void)arg;

    while (s_screen8_clock_run) {
        time_t now;
        time(&now);

        /* like weather_ui.cpp: if not synced (1970) -> do nothing */
        if (now > 1000000) {
            struct tm ti;
            localtime_r(&now, &ti);

            screen8_clock_msg_t *msg = (screen8_clock_msg_t*)malloc(sizeof(screen8_clock_msg_t));
            if (msg) {
                snprintf(msg->time_str, sizeof(msg->time_str), "%02d:%02d", ti.tm_hour, ti.tm_min);
                msg->year = ti.tm_year + 1900;
                msg->month = ti.tm_mon + 1;
                msg->day = ti.tm_mday;
                msg->wday = ti.tm_wday;

                if (!screen8_dispatch_async(screen8_clock_apply_cb, msg)) {
                    free(msg);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }

    s_screen8_clock_task = NULL;
    vTaskDelete(NULL);
}

static void screen8_clock_start(void)
{
    if (s_screen8_clock_task) return;

    s_screen8_clock_run = true;
    xTaskCreate(screen8_clock_task, "screen8_clock", 3072, NULL, 1, &s_screen8_clock_task);
}

static void screen8_clock_stop(void)
{
    if (!s_screen8_clock_task) return;

    s_screen8_clock_run = false;
    /* hard stop to avoid dangling task if screen destroyed immediately */
    vTaskDelete(s_screen8_clock_task);
    s_screen8_clock_task = NULL;
}

/* ===================== Booking / countdown state ===================== */

static bool        s_booking_active = false;
static int         s_selected_duration_sec = 30 * 60; // default 30 min
static int         s_remaining_sec = 0;
static int64_t     s_booking_deadline_us = 0;
static lv_timer_t *s_countdown_timer = NULL;
static lv_obj_t   *s_stop_confirm_msgbox = NULL;

static const char *k_stop_confirm_btns_en[] = { "Yes", "No", "" };
static const char *k_stop_confirm_btns_de[] = { "确定", "取消", "" };

// "All day" is now dynamic (until end-of-day). Track selection explicitly.
static bool        s_full_day_selected = false;

/* ===================== ONE-MIN WARNING ===================== */
static bool        s_one_min_active = false;
static bool        s_one_min_label_state = false;
static lv_timer_t *s_one_min_blink_timer = NULL;

/* Forward decls */
static void duration_buttons_refresh(void);
static void countdown_delete_timer(void);
static void booking_start_countdown(int seconds);
static void countdown_stop(void);
static void booking_finish_and_restore(void);
static int booking_get_remaining_sec(void);
static void restore_booking_state_ui(void);
static void booking_recalc_positions(void);
static void status_set_free(void);
static void status_set_booked(int remaining_sec);
static void booking_style_stop_confirm_msgbox(lv_obj_t *mb);
static void booking_show_stop_confirm(void);

static void booking_reset_overlays(void)
{
    if (ui_ManualPanel) {
        lv_obj_add_flag(ui_ManualPanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_BookingOverlay) {
        lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
    }
    if (ui_BookingDrawer) {
        lv_obj_add_flag(ui_BookingDrawer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_COVER, 0);
        lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_closed_x);
    }
    s_booking_open = false;
}

void ui_start_meeting_duration(int seconds)
{
    if (seconds <= 0) {
        return;
    }

    booking_recalc_positions();

    if (s_stop_confirm_msgbox) {
        lv_obj_del(s_stop_confirm_msgbox);
        s_stop_confirm_msgbox = NULL;
    }

    s_selected_duration_sec = seconds;
    s_full_day_selected = false;
    duration_buttons_refresh();
    booking_reset_overlays();
    booking_start_countdown(seconds);

    ESP_LOGI("ui_Screen8", "Started meeting via external command: %d seconds", seconds);
}

void ui_stop_meeting_now(void)
{
    if (s_stop_confirm_msgbox) {
        lv_obj_del(s_stop_confirm_msgbox);
        s_stop_confirm_msgbox = NULL;
    }

    booking_reset_overlays();

    if (!s_booking_active) {
        status_set_free();
        ESP_LOGI("ui_Screen8", "Stop command received while no active meeting");
        return;
    }

    booking_finish_and_restore();
    ESP_LOGI("ui_Screen8", "Meeting stopped via external command");
}

bool ui_is_booking_active(void)
{
    if (!s_booking_active) {
        return false;
    }

    int remaining_sec = booking_get_remaining_sec();
    if (remaining_sec <= 0) {
        booking_finish_and_restore();
        return false;
    }

    s_remaining_sec = remaining_sec;
    return true;
}

int ui_get_booking_remaining_sec_snapshot(void)
{
    if (!s_booking_active) {
        return 0;
    }

    return booking_get_remaining_sec();
}

/* ONE-MIN functions */
static void one_min_blink_cb(lv_timer_t *t)
{
    (void)t;
    if (!ui_TimerLabel) return;
    s_one_min_label_state = !s_one_min_label_state;

    if (s_one_min_label_state) {
        lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_WARNING), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void start_one_min_warning(void)
{
    if (s_one_min_active) return;
    s_one_min_active = true;
    s_one_min_label_state = false;

    /* Immediately set amber to attract attention */
    if (ui_TimerLabel) {
        lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_WARNING), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (!s_one_min_blink_timer) {
        s_one_min_blink_timer = lv_timer_create(one_min_blink_cb, 500, NULL);
        if (s_one_min_blink_timer) lv_timer_set_repeat_count(s_one_min_blink_timer, -1);
    }
}

static void stop_one_min_warning(void)
{
    if (!s_one_min_active) {
        if (s_one_min_blink_timer) {
            lv_timer_del(s_one_min_blink_timer);
            s_one_min_blink_timer = NULL;
        }
        return;
    }
    s_one_min_active = false;
    s_one_min_label_state = false;

    if (s_one_min_blink_timer) {
        lv_timer_del(s_one_min_blink_timer);
        s_one_min_blink_timer = NULL;
    }

    if (ui_TimerLabel) {
        lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* ===================== Utils ===================== */

static void format_remaining(int sec, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (sec < 0) sec = 0;

    /* FIX: use >= 3600 so 3600 -> 1:00:00, not 60:00 */
    if (sec >= 3600) {
        int h = sec / 3600;
        int m = (sec % 3600) / 60;
        int s = sec % 60;
        snprintf(out, out_sz, "%d:%02d:%02d", h, m, s);
    } else {
        int mm = sec / 60;
        int ss = sec % 60;
        snprintf(out, out_sz, "%02d:%02d", mm, ss);
    }
}

static int seconds_until_end_of_day_local(void)
{
    time_t now = 0;
    time(&now);
    if (now <= 0) return 24 * 60 * 60;

    struct tm lt;
    localtime_r(&now, &lt);

    // Today 23:59:59 local time
    struct tm eod = lt;
    eod.tm_hour = 23;
    eod.tm_min  = 59;
    eod.tm_sec  = 59;

    time_t eod_epoch = mktime(&eod);
    if (eod_epoch <= 0) {
        return 24 * 60 * 60;
    }

    if (eod_epoch <= now) return 1;

    long diff = (long)(eod_epoch - now);
    if (diff < 1) diff = 1;
    if (diff > 24L * 60L * 60L) diff = 24L * 60L * 60L;

    return (int)diff;
}

/* safer estimate for modal sizing across displays */
static void booking_recalc_positions(void)
{
    int screen_w = 800;
    lv_coord_t main_x = 0;
    lv_coord_t main_y = 0;
    lv_coord_t main_w = 0;
    lv_coord_t main_h = 0;

    screen8_get_display_size(&screen_w, NULL);
    screen8_get_main_area(&main_x, &main_y, &main_w, &main_h);

    s_open_x = main_x + main_w - s_drawer_w;
    if (s_open_x < main_x) s_open_x = main_x;
    s_closed_x = s_open_x;

    if (ui_BookingOverlay) {
        lv_obj_set_pos(ui_BookingOverlay, 0, main_y);
        lv_obj_set_size(ui_BookingOverlay, screen_w, main_h);
    }

    if (ui_BookingDrawer) {
        lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_open_x);
        lv_obj_set_y(ui_BookingDrawer, main_y);
        lv_obj_set_height(ui_BookingDrawer, main_h);
        if (!s_booking_open) {
            lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_COVER, 0);
        }
    }
}

/* ===================== Helpers to avoid stack overflow ===================== */

static char *s_hours_opts = NULL;
static char *s_mins_opts = NULL;

/* safer build_options: compute required buffer size precisely and use snprintf */
static char * build_options(int start, int end)
{
    if (end < start) return NULL;
    int count = end - start + 1;

    /* compute max digits needed for end */
    int max_val = end;
    int max_digits = 1;
    while (max_val >= 10) { max_digits++; max_val /= 10; }

    /* each entry: digits + separator ('\n' or '\0') */
    size_t est = (size_t)count * (max_digits + 1) + 1;
    char *buf = (char*)malloc(est);
    if (!buf) return NULL;

    char *p = buf;
    size_t remain = est;
    for (int i = start; i <= end; ++i) {
        if (i != start) {
            if (remain <= 1) break;
            *p++ = '\n';
            remain--;
        }
        int n = snprintf(p, remain, "%d", i);
        if (n < 0 || (size_t)n >= remain) break;
        p += n;
        remain -= (size_t)n;
    }
    *p = '\0';
    return buf;
}

static char * build_options_padded(int start, int end)
{
    if (end < start) return NULL;
    int count = end - start + 1;
    int max_digits = 2; /* we expect padded to 2 digits here (00..59) */
    size_t est = (size_t)count * (max_digits + 1) + 1;
    char *buf = (char*)malloc(est);
    if (!buf) return NULL;

    char *p = buf;
    size_t remain = est;
    for (int i = start; i <= end; ++i) {
        if (i != start) {
            if (remain <= 1) break;
            *p++ = '\n';
            remain--;
        }
        int n = snprintf(p, remain, "%02d", i);
        if (n < 0 || (size_t)n >= remain) break;
        p += n;
        remain -= (size_t)n;
    }
    *p = '\0';
    return buf;
}

static void style_roller_dark(lv_obj_t *roller)
{
    if (!roller) return;
    lv_obj_set_style_bg_color(roller, lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(roller, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, lv_color_hex(SCREEN8_COLOR_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(roller, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(roller, lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(roller, 0, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(roller, &ui_font_Roboto26, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_PRESSED);
}

/* Large title font for the free-status card.
 * Chinese glyphs are unavailable in Roboto50, so use the Chinese subset font there. */
static const lv_font_t *start_meeting_font(void)
{
    return language_get_en() ? &ui_font_Roboto50 : &ui_font_FontZhh50;
}

/* ===================== Language switching ===================== */

/* Apply language strings to UI (called from language callback) */
static void apply_language_to_ui(bool en)
{
    if (en) {
        if (ui_Label44) lv_label_set_text(ui_Label44, "Start meeting");
        if (ui_Label5) lv_label_set_text(ui_Label5, "Booking room");
        if (ui_Label7) lv_label_set_text(ui_Label7, "Time for booking:");
        if (ui_Label47) lv_label_set_text(ui_Label47, "15 min");
        if (ui_Label48) lv_label_set_text(ui_Label48, "30 min");
        if (ui_Label49) lv_label_set_text(ui_Label49, "45 min");
        if (ui_Label50) lv_label_set_text(ui_Label50, "1 hour");
        if (ui_LabelFullDay) lv_label_set_text(ui_LabelFullDay, "All day");
        if (ui_LabelManual) lv_label_set_text(ui_LabelManual, "Manual time");
        if (ui_Label51) lv_label_set_text(ui_Label51, "Start now");
        if (ui_Label38) lv_label_set_text(ui_Label38, "Info");
        if (ui_Label45) lv_label_set_text(ui_Label45, "Weather");
        if (ui_ManualCancelLbl) lv_label_set_text(ui_ManualCancelLbl, "Cancel");
        if (ui_ManualConfirmLbl) lv_label_set_text(ui_ManualConfirmLbl, "OK");
    } else {
        if (ui_Label44) lv_label_set_text(ui_Label44, "开始会议");
        if (ui_Label5) lv_label_set_text(ui_Label5, "预订会议室");
        if (ui_Label7) lv_label_set_text(ui_Label7, "预订时长:");
        if (ui_Label47) lv_label_set_text(ui_Label47, "15 Min");
        if (ui_Label48) lv_label_set_text(ui_Label48, "30 Min");
        if (ui_Label49) lv_label_set_text(ui_Label49, "45 Min");
        if (ui_Label50) lv_label_set_text(ui_Label50, "1小时");
        if (ui_LabelFullDay) lv_label_set_text(ui_LabelFullDay, "全天");
        if (ui_LabelManual) lv_label_set_text(ui_LabelManual, "自定义时间");
        if (ui_Label51) lv_label_set_text(ui_Label51, "立即开始");
        if (ui_Label38) lv_label_set_text(ui_Label38, "信息");
        if (ui_Label45) lv_label_set_text(ui_Label45, "天气");
        if (ui_ManualCancelLbl) lv_label_set_text(ui_ManualCancelLbl, "取消");
        if (ui_ManualConfirmLbl) lv_label_set_text(ui_ManualConfirmLbl, "确定");
    }

    if (ui_Label44) {
        lv_obj_set_style_text_font(ui_Label44, start_meeting_font(), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* language animation callbacks (unchanged except toggling now via language_manager) */
static void anim_lang_down_ready_cb(lv_anim_t *a) { (void)a; s_lang_anim_running = false; }

static void anim_lang_up_ready_cb(lv_anim_t *a)
{
    (void)a;

    /* Toggle language via language manager */
    language_toggle();

    /* Update LangLabel text to reflect new language immediately (language_register_callback also updates other labels) */
    if (ui_LangLabel) lv_label_set_text(ui_LangLabel, language_get_en() ? "EN" : "ZH");

    if (!ui_LangLabel) { s_lang_anim_running = false; return; }
    lv_coord_t cur_y = lv_obj_get_y(ui_LangLabel);
    lv_coord_t target = (s_lang_label_y_orig == LV_COORD_MIN) ? (cur_y + 12) : s_lang_label_y_orig;

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, ui_LangLabel);
    lv_anim_set_exec_cb(&b, anim_y_cb);
    lv_anim_set_values(&b, (int)cur_y, (int)target);
    lv_anim_set_time(&b, 260);
    lv_anim_set_path_cb(&b, lv_anim_path_overshoot);
    lv_anim_set_ready_cb(&b, anim_lang_down_ready_cb);
    lv_anim_start(&b);
}

/* UI event for language switch: animates label up and ready callback toggles language */
static void ui_event_LangSwitch(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!ui_LangLabel || s_lang_anim_running) return;
    s_lang_anim_running = true;
    lv_coord_t orig_y = (s_lang_label_y_orig == LV_COORD_MIN) ? lv_obj_get_y(ui_LangLabel) : s_lang_label_y_orig;
    lv_coord_t up = orig_y - 12;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_LangLabel);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, (int)orig_y, (int)up);
    lv_anim_set_time(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, anim_lang_up_ready_cb);
    lv_anim_start(&a);
}

/* ===================== Events (navigation) ===================== */

void ui_event_Panel2(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _ui_screen_change(&ui_ScreenInfo, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_ScreenInfo_screen_init);
    }
}

void ui_event_settings(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen1_screen_init);
    }
}

/* bottom Weather = same logic as click on right widget */
static void ui_event_BottomWeather(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_weather_widget) {
        lv_event_send(s_weather_widget, LV_EVENT_CLICKED, NULL);
        return;
    }
}

/* ===================== Events (booking UI) ===================== */

static void booking_stop_confirm_event_cb(lv_event_t * e)
{
    lv_obj_t *mb = lv_event_get_current_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (!mb) return;

    if (code == LV_EVENT_DELETE) {
        if (mb == s_stop_confirm_msgbox) s_stop_confirm_msgbox = NULL;
        return;
    }

    if (code != LV_EVENT_VALUE_CHANGED) return;

    uint16_t btn_id = lv_msgbox_get_active_btn(mb);
    if (btn_id == LV_BTNMATRIX_BTN_NONE) return;

    if (btn_id == 0) {
        booking_finish_and_restore();
    }

    lv_msgbox_close_async(mb);
}

static void booking_style_stop_confirm_msgbox(lv_obj_t *mb)
{
    if (!mb) return;

    lv_obj_t *title = lv_msgbox_get_title(mb);
    lv_obj_t *text = lv_msgbox_get_text(mb);
    lv_obj_t *content = lv_msgbox_get_content(mb);
    lv_obj_t *close_btn = lv_msgbox_get_close_btn(mb);
    lv_obj_t *btns = lv_msgbox_get_btns(mb);

    lv_obj_set_size(mb, 460, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(mb, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(mb, lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(mb, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(mb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(mb, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(mb, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(mb, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(mb, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(mb, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(mb, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(mb, 44, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_x(mb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(mb, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(mb, lv_color_hex(SCREEN8_COLOR_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);

    if (content) {
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(content, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(content, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (title) {
        lv_obj_set_style_text_font(title, &ui_font_Roboto30, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(title, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (text) {
        lv_obj_set_style_text_font(text, &ui_font_Roboto20, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(text, lv_color_hex(SCREEN8_COLOR_TEXT_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (close_btn) {
        lv_obj_set_size(close_btn, 54, 54);
        lv_obj_set_style_radius(close_btn, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(close_btn, lv_color_hex(SCREEN8_COLOR_BUTTON), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(close_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(close_btn, lv_color_hex(SCREEN8_COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *close_label = lv_obj_get_child(close_btn, 0);
        if (close_label) {
            lv_label_set_text(close_label, "X");
            lv_obj_set_style_text_font(close_label, &ui_font_Roboto20, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(close_label, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    if (btns) {
        lv_obj_set_size(btns, LV_PCT(100), 84);
        lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_width(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(btns, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_row(btns, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_clip_corner(btns, true, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_set_style_radius(btns, 16, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btns, lv_color_hex(SCREEN8_COLOR_BUTTON), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btns, 255, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btns, lv_color_hex(SCREEN8_COLOR_BUTTON_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(btns, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_width(btns, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(btns, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(btns, 0, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_outline_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_outline_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(btns, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btns, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(btns, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btns, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(btns, &ui_font_Roboto20, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(btns, 14, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(btns, 14, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(btns, 12, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(btns, 12, LV_PART_ITEMS | LV_STATE_DEFAULT);
    }
}

static void booking_show_stop_confirm(void)
{
    bool en;
    const char *title;
    const char *text;
    lv_obj_t *parent;

    if (s_stop_confirm_msgbox) {
        lv_obj_move_foreground(s_stop_confirm_msgbox);
        return;
    }

    en = language_get_en();
    title = en ? "Stop meeting" : "停止会议";
    text = en ? "Stop the current meeting?" : "停止当前会议?";
    parent = lv_layer_top();
    if (!parent) parent = lv_scr_act();

    s_stop_confirm_msgbox = lv_msgbox_create(
        parent,
        title,
        text,
        en ? k_stop_confirm_btns_en : k_stop_confirm_btns_de,
        true
    );

    if (!s_stop_confirm_msgbox) return;

    booking_style_stop_confirm_msgbox(s_stop_confirm_msgbox);
    lv_obj_center(s_stop_confirm_msgbox);
    lv_obj_move_foreground(s_stop_confirm_msgbox);
    lv_obj_add_event_cb(s_stop_confirm_msgbox, booking_stop_confirm_event_cb, LV_EVENT_ALL, NULL);
}

static void ui_event_Panel4(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_booking_active) {
        booking_show_stop_confirm();
        return;
    }
    booking_set_visible(true, true);
}

static void ui_event_BookingClose(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_CLICKED && code != LV_EVENT_SHORT_CLICKED) return;
    if (!s_booking_open) return;
    booking_set_visible(false, true);
}

static void ui_event_DurationBtn(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t sec = (intptr_t)lv_event_get_user_data(e);
    if (sec <= 0) return;
    s_selected_duration_sec = (int)sec;
    s_full_day_selected = false;
    duration_buttons_refresh();
}

static void ui_event_FullDay(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_full_day_selected = true;
    s_selected_duration_sec = seconds_until_end_of_day_local();

    ESP_LOGI("ui_Screen8", "All day selected -> %d sec until end-of-day", s_selected_duration_sec);

    duration_buttons_refresh();
}

static void ui_event_ManualOpen(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!ui_ManualPanel) return;
    if (ui_BookingOverlay) {
        lv_obj_clear_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_BookingOverlay);
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 110, 0);
    }
    lv_obj_clear_flag(ui_ManualPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_ManualPanel);
    if (ui_HoursSelFrame) lv_obj_move_foreground(ui_HoursSelFrame);
    if (ui_MinsSelFrame)  lv_obj_move_foreground(ui_MinsSelFrame);
}
/* Image switch handler on press/release */
static void ui_event_ImgBtn(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);
    /* assume: first (and only) button child is the image */
    lv_obj_t * img = lv_obj_get_child(btn, 0);

    if(code == LV_EVENT_PRESSED) {
        /* show pressed image (replace resource if available) */
        lv_img_set_src(img, &ui_img_settingsbuttonan_png);
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* restore released image */
        lv_img_set_src(img, &ui_img_settingsbuttonan_png);
    }
    else if(code == LV_EVENT_CLICKED) {
        /* handle click here, e.g. call a function */
        /* ui_event_on_imgbtn_click(); */
    }
}

static void ui_event_ManualConfirm(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!ui_RollerHours || !ui_RollerMins) return;
    uint16_t h_idx = lv_roller_get_selected(ui_RollerHours);
    uint16_t m_idx = lv_roller_get_selected(ui_RollerMins);
    int seconds = (int)h_idx * 3600 + (int)m_idx * 60;
    if (seconds <= 0) seconds = 60;
    s_selected_duration_sec = seconds;
    s_full_day_selected = false;
    duration_buttons_refresh();
    if (ui_ManualPanel) lv_obj_add_flag(ui_ManualPanel, LV_OBJ_FLAG_HIDDEN);
    if (ui_BookingOverlay) {
        lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
    }
    if (s_booking_open) booking_set_visible(false, true);
    booking_start_countdown(s_selected_duration_sec);
}

static void ui_event_ManualCancel(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (ui_ManualPanel) lv_obj_add_flag(ui_ManualPanel, LV_OBJ_FLAG_HIDDEN);
    if (!ui_BookingOverlay) return;
    if (!s_booking_open) {
        lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 110, 0);
        if (ui_BookingDrawer) lv_obj_move_foreground(ui_BookingDrawer);
    }
}

static void ui_event_StartNow(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected_duration_sec <= 0) s_selected_duration_sec = 30 * 60;
    booking_set_visible(false, true);
    booking_start_countdown(s_selected_duration_sec);
}

/* ===================== Drawer helpers ===================== */

static void booking_hide_ready_cb(lv_anim_t * a)
{
    (void)a;
    if (ui_BookingDrawer)  lv_obj_add_flag(ui_BookingDrawer,  LV_OBJ_FLAG_HIDDEN);
    if (ui_BookingOverlay) lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
    booking_recalc_positions();
    if (ui_BookingDrawer) lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_closed_x);
    if (ui_BookingDrawer) lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_COVER, 0);
    if (ui_BookingOverlay) lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
}

static void booking_set_visible(bool open, bool anim)
{
    if (!ui_BookingDrawer || !ui_BookingOverlay) return;
    booking_recalc_positions();
    lv_anim_del(ui_BookingDrawer, anim_x_cb);
    lv_anim_del(ui_BookingDrawer, anim_obj_opa_cb);
    lv_anim_del(ui_BookingOverlay, anim_opa_cb);
    if (open == s_booking_open) return;
    s_booking_open = open;
    if (open) {
        lv_obj_clear_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_BookingOverlay);
        lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
        lv_obj_clear_flag(ui_BookingDrawer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_BookingDrawer);
        lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_open_x);
        lv_coord_t main_y = 0;
        screen8_get_main_area(NULL, &main_y, NULL, NULL);
        lv_obj_set_y(ui_BookingDrawer, main_y);
        lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_TRANSP, 0);
        if (!anim) {
            lv_obj_set_style_bg_opa(ui_BookingOverlay, 110, 0);
            lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_COVER, 0);
            return;
        }
        lv_anim_t o;
        lv_anim_init(&o);
        lv_anim_set_var(&o, ui_BookingOverlay);
        lv_anim_set_exec_cb(&o, anim_opa_cb);
        lv_anim_set_time(&o, BOOKING_SHOW_OVERLAY_ANIM_MS);
        lv_anim_set_path_cb(&o, lv_anim_path_ease_out);
        lv_anim_set_values(&o, 0, 110);
        lv_anim_start(&o);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_BookingDrawer);
        lv_anim_set_exec_cb(&a, anim_obj_opa_cb);
        lv_anim_set_time(&a, BOOKING_SHOW_DRAWER_ANIM_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_start(&a);
    } else {
        if (!anim) {
            lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_closed_x);
            lv_obj_set_style_opa(ui_BookingDrawer, LV_OPA_COVER, 0);
            lv_obj_add_flag(ui_BookingDrawer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
            return;
        }
        lv_anim_t o;
        lv_anim_init(&o);
        lv_anim_set_var(&o, ui_BookingOverlay);
        lv_anim_set_exec_cb(&o, anim_opa_cb);
        lv_anim_set_time(&o, BOOKING_HIDE_OVERLAY_ANIM_MS);
        lv_anim_set_path_cb(&o, lv_anim_path_ease_in);
        lv_anim_set_values(&o, 110, 0);
        lv_anim_start(&o);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_BookingDrawer);
        lv_anim_set_exec_cb(&a, anim_obj_opa_cb);
        lv_anim_set_time(&a, BOOKING_HIDE_DRAWER_ANIM_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_ready_cb(&a, booking_hide_ready_cb);
        lv_anim_start(&a);
    }
}

/* ===================== Status card UI states ===================== */

static void status_set_free(void)
{
    s_booking_active = false;
    countdown_stop();
    if (ui_Panel4) {
        lv_obj_set_size(ui_Panel4, SCREEN8_LEFT_COL_W, screen8_get_left_card_height());
        lv_obj_set_style_bg_color(ui_Panel4, lv_color_hex(SCREEN8_COLOR_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_Panel4, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_flex_align(ui_Panel4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(ui_Panel4, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_row(ui_Panel4, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ui_Panel4, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(ui_Panel4, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(ui_Panel4, lv_color_hex(SCREEN8_COLOR_ACCENT_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_ofs_y(ui_Panel4, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (ui_Label44) {
        lv_label_set_text(ui_Label44, language_get_en() ? "Start meeting" : "开始会议");
        lv_obj_set_style_text_font(ui_Label44, start_meeting_font(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_Label44, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(ui_Label44, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_TimerLabel) lv_obj_add_flag(ui_TimerLabel, LV_OBJ_FLAG_HIDDEN);
    stop_one_min_warning();
}

static void status_set_booked(int remaining_sec)
{
    s_booking_active = true;
    if (ui_Panel4) {
        lv_obj_set_size(ui_Panel4, SCREEN8_LEFT_COL_W, screen8_get_left_card_height());
        lv_obj_set_style_bg_color(ui_Panel4, lv_color_hex(SCREEN8_COLOR_OCCUPIED_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_Panel4, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Panel4, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Panel4, lv_color_hex(SCREEN8_COLOR_OCCUPIED_BORDER), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(ui_Panel4, 220, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_flex_align(ui_Panel4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(ui_Panel4, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_row(ui_Panel4, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ui_Panel4, 28, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(ui_Panel4, 42, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(ui_Panel4, lv_color_hex(SCREEN8_COLOR_OCCUPIED_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_ofs_y(ui_Panel4, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (ui_Label44) {
        lv_obj_add_flag(ui_Label44, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_TimerLabel) {
        char buf[32];
        format_remaining(remaining_sec, buf, sizeof(buf));
        lv_label_set_text(ui_TimerLabel, buf);
        lv_obj_clear_flag(ui_TimerLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/* ===================== Duration buttons style ===================== */

static void set_duration_btn_style(lv_obj_t *btn, bool selected)
{
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, selected ? lv_color_hex(SCREEN8_COLOR_ACCENT) : lv_color_hex(SCREEN8_COLOR_BUTTON),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, selected ? 255 : 210, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, selected ? 12 : 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(btn, selected ? 18 : 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(SCREEN8_COLOR_ACCENT_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(btn, selected ? 5 : 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn,
                                selected ? lv_color_hex(SCREEN8_COLOR_TEXT) : lv_color_hex(SCREEN8_COLOR_TEXT_MUTED),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
}

static void duration_buttons_refresh(void)
{
    set_duration_btn_style(ui_Button2, s_selected_duration_sec == 15 * 60);
    set_duration_btn_style(ui_Button3, s_selected_duration_sec == 30 * 60);
    set_duration_btn_style(ui_Button4, s_selected_duration_sec == 45 * 60);
    set_duration_btn_style(ui_Button5, s_selected_duration_sec == 60 * 60);
    set_duration_btn_style(ui_ButtonFullDay, s_full_day_selected);
    bool manual_selected = (!s_full_day_selected &&
                            s_selected_duration_sec != 15*60 && s_selected_duration_sec != 30*60 &&
                            s_selected_duration_sec != 45*60 && s_selected_duration_sec != 60*60);
    set_duration_btn_style(ui_ButtonManual, manual_selected);
}

/* ===================== Countdown engine ===================== */

static void countdown_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_booking_active) {
        countdown_delete_timer();
        return;
    }

    int remaining_sec = booking_get_remaining_sec();
    if (remaining_sec <= 0) {
        booking_finish_and_restore();
        return;
    }

    s_remaining_sec = remaining_sec;
    status_set_booked(s_remaining_sec);
    if (s_remaining_sec <= 60) {
        start_one_min_warning();
    } else if (s_one_min_active) {
        stop_one_min_warning();
    }
}

static void countdown_delete_timer(void)
{
    if (s_countdown_timer) {
        lv_timer_del(s_countdown_timer);
        s_countdown_timer = NULL;
    }
}

static void countdown_stop(void)
{
    countdown_delete_timer();
    s_remaining_sec = 0;
    s_booking_deadline_us = 0;
    stop_one_min_warning();
}

static int booking_get_remaining_sec(void)
{
    if (!s_booking_active || s_booking_deadline_us <= 0) {
        return 0;
    }

    int64_t remaining_us = s_booking_deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }

    return (int)((remaining_us + 999999LL) / 1000000LL);
}

static void booking_start_countdown(int seconds)
{
    if (seconds <= 0) return;

    countdown_delete_timer();
    s_booking_active = true;
    s_booking_deadline_us = esp_timer_get_time() + ((int64_t)seconds * 1000000LL);
    s_remaining_sec = booking_get_remaining_sec();
    if (s_remaining_sec <= 0) {
        s_remaining_sec = seconds;
    }
    status_set_booked(s_remaining_sec);
    s_countdown_timer = lv_timer_create(countdown_tick_cb, 1000, NULL);
    if (s_countdown_timer) lv_timer_set_repeat_count(s_countdown_timer, -1);
}

static void booking_finish_and_restore(void)
{
    if (s_booking_open) booking_set_visible(false, true);
    countdown_stop();
    stop_one_min_warning();
    s_full_day_selected = false;
    s_selected_duration_sec = 30 * 60;
    status_set_free();
}

static void restore_booking_state_ui(void)
{
    int remaining_sec = booking_get_remaining_sec();
    booking_reset_overlays();

    if (remaining_sec <= 0) {
        if (s_booking_active) {
            booking_finish_and_restore();
        } else {
            status_set_free();
        }
        return;
    }

    s_booking_active = true;
    s_remaining_sec = remaining_sec;
    status_set_booked(remaining_sec);

    if (remaining_sec <= 60) {
        start_one_min_warning();
    } else {
        stop_one_min_warning();
    }

    if (!s_countdown_timer) {
        s_countdown_timer = lv_timer_create(countdown_tick_cb, 1000, NULL);
        if (s_countdown_timer) lv_timer_set_repeat_count(s_countdown_timer, -1);
    }
}

static void ui_update_events_labels(void) {
    /* Event card removed intentionally */
}

/* ===================== Language callback registration for this screen ============= */
/* We'll store the callback id so we can unregister on destroy */
static int s_lang_cb_id = -1;

/* this is called when language changes */
static void ui_language_changed_cb(bool en, void *ctx)
{
    (void)ctx;
    /* apply strings to UI (safe to call even if some pointers are NULL) */
    apply_language_to_ui(en);

    /* ensure LangLabel shows the current short code */
    if (ui_LangLabel) lv_label_set_text(ui_LangLabel, en ? "EN" : "ZH");

    screen8_refresh_clock_now();
    ui_update_events_labels();
    weather_integration_refresh_language();
}

/* ===================== WiFi icon helper ===================== */

/* check connection state and show/hide icon accordingly */
static void wifi_icon_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_Screen8_update_wifi_icon();
}

/* public helper - make it callable from other screens or wifi event handlers */
void ui_Screen8_update_wifi_icon(void)
{
    if (!ui_WifiIcon) return;
    if (wifi_manager_is_connected()) {
        lv_obj_clear_flag(ui_WifiIcon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_WifiIcon, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_Screen8_refresh_device_name(void)
{
    if (!ui_Label4) {
        return;
    }

    lv_label_set_text(ui_Label4, device_name_store_get());
}

/* ===================== Screen init ===================== */

void ui_Screen8_screen_init(void)
{
    lv_coord_t main_y = 0;
    lv_coord_t main_w = 0;
    lv_coord_t main_h = 0;
    int screen_w = 800;

    language_init(language_get_en());
    screen8_get_display_size(&screen_w, NULL);
    screen8_get_main_area(NULL, &main_y, &main_w, &main_h);

    lv_coord_t right_col_w = main_w - SCREEN8_LEFT_COL_W - SCREEN8_MAIN_GAP;
    if (right_col_w < 390) right_col_w = 390;
    lv_coord_t footer_item_w = (main_w - 24 - SCREEN8_MAIN_GAP) / 2;
    if (footer_item_w < 220) footer_item_w = 220;
    lv_coord_t left_card_h = screen8_get_left_card_height();

    ui_Screen8 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen8, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui_Screen8, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_Screen8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(ui_Screen8, SCREEN8_OUTER_PAD_Y, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Screen8, SCREEN8_OUTER_PAD_Y, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_Screen8, SCREEN8_SECTION_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Screen8, lv_color_hex(SCREEN8_COLOR_BG_TOP), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(ui_Screen8, lv_color_hex(SCREEN8_COLOR_BG_BOTTOM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui_Screen8, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen8, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_src(ui_Screen8, NULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_opa(ui_Screen8, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Container2 = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_Container2);
    lv_obj_set_size(ui_Container2, main_w, SCREEN8_HEADER_H);
    lv_obj_set_flex_flow(ui_Container2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Container2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(ui_Container2, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Container2, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Container2, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Container2, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ui_Container2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Container2, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    screen8_style_surface_card(ui_Container2, lv_color_hex(SCREEN8_COLOR_SURFACE_ALT), 245,
                               lv_color_hex(SCREEN8_COLOR_SURFACE_ALT), 20);

    lv_obj_t *header_left = lv_obj_create(ui_Container2);
    lv_obj_remove_style_all(header_left);
    lv_obj_set_size(header_left, 104, lv_pct(100));
    lv_obj_set_flex_flow(header_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_left, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(header_left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_LangSwitch = lv_obj_create(header_left);
    lv_obj_remove_style_all(ui_LangSwitch);
    lv_obj_set_size(ui_LangSwitch, 56, 32);
    lv_obj_add_flag(ui_LangSwitch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(ui_LangSwitch, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(ui_LangSwitch, LV_SCROLLBAR_MODE_OFF);
    screen8_style_header_button(ui_LangSwitch);
    lv_obj_add_event_cb(ui_LangSwitch, ui_event_LangSwitch, LV_EVENT_CLICKED, NULL);

    ui_LangLabel = lv_label_create(ui_LangSwitch);
    lv_label_set_text(ui_LangLabel, language_get_en() ? "EN" : "ZH");
    lv_obj_center(ui_LangLabel);
    lv_obj_set_style_text_font(ui_LangLabel, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LangLabel, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    s_lang_label_y_orig = lv_obj_get_y(ui_LangLabel);

    ui_WifiIcon = lv_img_create(header_left);
    lv_img_set_src(ui_WifiIcon, &ui_img_wifi32_png);
    lv_obj_add_flag(ui_WifiIcon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *header_center = lv_obj_create(ui_Container2);
    lv_obj_remove_style_all(header_center);
    lv_obj_set_size(header_center, 0, lv_pct(100));
    lv_obj_set_flex_grow(header_center, 1);
    lv_obj_set_flex_flow(header_center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header_center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_Label4 = lv_label_create(header_center);
    lv_label_set_text(ui_Label4, device_name_store_get());
    lv_label_set_long_mode(ui_Label4, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui_Label4, lv_pct(100));
    lv_obj_set_style_text_align(ui_Label4, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label4, &ui_font_Roboto26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Label4, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *header_right = lv_obj_create(ui_Container2);
    lv_obj_remove_style_all(header_right);
    lv_obj_set_size(header_right, 104, lv_pct(100));
    lv_obj_set_flex_flow(header_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_right, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(header_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_ImgBtn = lv_btn_create(header_right);
    lv_obj_remove_style_all(ui_ImgBtn);
    lv_obj_set_size(ui_ImgBtn, 40, 40);
    lv_obj_set_style_pad_all(ui_ImgBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_ImgBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ui_ImgBtn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_ext_click_area(ui_ImgBtn, 8);
    screen8_style_header_button(ui_ImgBtn);

    ui_ImgBtnImg = lv_img_create(ui_ImgBtn);
    lv_img_set_src(ui_ImgBtnImg, &ui_img_settingsbuttonan_png);
    lv_obj_center(ui_ImgBtnImg);
    lv_obj_add_event_cb(ui_ImgBtn, ui_event_ImgBtn, LV_EVENT_ALL, NULL);

    s_wifi_icon_timer = lv_timer_create(wifi_icon_timer_cb, 2000, NULL);
    if (s_wifi_icon_timer) lv_timer_set_repeat_count(s_wifi_icon_timer, -1);
    wifi_icon_timer_cb(NULL);

    ui_Container4 = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_Container4);
    lv_obj_set_size(ui_Container4, main_w, main_h);
    lv_obj_set_flex_flow(ui_Container4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Container4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_Container4, SCREEN8_MAIN_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Container4, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_Container8 = lv_obj_create(ui_Container4);
    lv_obj_remove_style_all(ui_Container8);
    lv_obj_set_size(ui_Container8, SCREEN8_LEFT_COL_W, lv_pct(100));
    lv_obj_set_flex_flow(ui_Container8, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_Container8, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_Container8, SCREEN8_MAIN_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Container8, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_Container26 = lv_obj_create(ui_Container8);
    lv_obj_remove_style_all(ui_Container26);
    lv_obj_set_size(ui_Container26, lv_pct(100), left_card_h);
    lv_obj_set_flex_flow(ui_Container26, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_Container26, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ui_Container26, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_Container26, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Container26, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    screen8_style_surface_card(ui_Container26, lv_color_hex(SCREEN8_COLOR_SURFACE), 245,
                               lv_color_hex(SCREEN8_COLOR_SURFACE), SCREEN8_CARD_RADIUS);

    ui_Label41 = lv_label_create(ui_Container26);
    lv_label_set_text(ui_Label41, "--:--");
    lv_obj_set_width(ui_Label41, SCREEN8_LEFT_TEXT_W);
    lv_obj_set_style_text_color(ui_Label41, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label41, &ui_font_Roboto60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui_Label41, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label41, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label42 = lv_label_create(ui_Container26);
    lv_label_set_text(ui_Label42, "---");
    lv_obj_set_width(ui_Label42, SCREEN8_LEFT_TEXT_W);
    lv_obj_set_style_text_color(ui_Label42, lv_color_hex(SCREEN8_COLOR_TEXT_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label42, &ui_font_Roboto30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui_Label42, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label42, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    {
        time_t now;
        time(&now);
        if (now > 1000000) {
            screen8_refresh_clock_now();
        }
    }
    screen8_clock_start();

    ui_Panel4 = lv_obj_create(ui_Container8);
    lv_obj_remove_style_all(ui_Panel4);
    lv_obj_set_size(ui_Panel4, SCREEN8_LEFT_COL_W, left_card_h);
    lv_obj_set_flex_flow(ui_Panel4, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_Panel4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ui_Panel4, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_Panel4, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Panel4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_Panel4, LV_OBJ_FLAG_CLICKABLE);
    screen8_style_surface_card(ui_Panel4, lv_color_hex(SCREEN8_COLOR_ACCENT), 255,
                               lv_color_hex(SCREEN8_COLOR_ACCENT), SCREEN8_CARD_RADIUS);

    ui_Label44 = lv_label_create(ui_Panel4);
    lv_label_set_text(ui_Label44, language_get_en() ? "Start meeting" : "开始会议");
    lv_obj_set_width(ui_Label44, SCREEN8_STATUS_TEXT_W);
    lv_label_set_long_mode(ui_Label44, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui_Label44, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label44, start_meeting_font(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label44, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_TimerLabel = lv_label_create(ui_Panel4);
    lv_label_set_text(ui_TimerLabel, "00:00");
    lv_obj_set_width(ui_TimerLabel, SCREEN8_LEFT_TEXT_W);
    lv_obj_set_style_text_color(ui_TimerLabel, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_TimerLabel, &ui_font_Roboto60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui_TimerLabel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_TimerLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_TimerLabel, LV_OBJ_FLAG_HIDDEN);

    ui_Container25 = lv_obj_create(ui_Container4);
    lv_obj_remove_style_all(ui_Container25);
    lv_obj_set_size(ui_Container25, right_col_w, lv_pct(100));
    lv_obj_clear_flag(ui_Container25, LV_OBJ_FLAG_SCROLLABLE);

    static bool s_weather_started_once = false;
    if (!s_weather_started_once) {
        s_weather_started_once = true;
        weather_integration_init();
    }

    s_weather_widget = weather_integration_create_widget(ui_Container25);
    if (s_weather_widget) {
        lv_obj_center(s_weather_widget);
        lv_obj_move_foreground(s_weather_widget);
    }

    ui_BookingOverlay = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_BookingOverlay);
    lv_obj_set_size(ui_BookingOverlay, screen_w, main_h);
    lv_obj_set_pos(ui_BookingOverlay, 0, main_y);
    lv_obj_add_flag(ui_BookingOverlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_BookingOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_BookingOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ui_BookingOverlay, 0, 0);
    lv_obj_add_event_cb(ui_BookingOverlay, ui_event_BookingClose, LV_EVENT_CLICKED, NULL);

    ui_BookingDrawer = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_BookingDrawer);
    lv_obj_set_size(ui_BookingDrawer, s_drawer_w, main_h);
    lv_obj_set_pos(ui_BookingDrawer, screen_w, main_y);
    lv_obj_add_flag(ui_BookingDrawer, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_BookingDrawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui_BookingDrawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_BookingDrawer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(ui_BookingDrawer, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_BookingDrawer, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    screen8_style_surface_card(ui_BookingDrawer, lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), 250,
                               lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), 22);
    lv_obj_set_style_shadow_width(ui_BookingDrawer, 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui_BookingDrawer, 42, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_x(ui_BookingDrawer, -4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(ui_BookingDrawer, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_BookingHeader = lv_obj_create(ui_BookingDrawer);
    lv_obj_remove_style_all(ui_BookingHeader);
    lv_obj_set_size(ui_BookingHeader, lv_pct(100), 50);
    lv_obj_set_style_border_width(ui_BookingHeader, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ui_BookingHeader, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_BookingHeader, lv_color_hex(SCREEN8_COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label5 = lv_label_create(ui_BookingHeader);
    lv_obj_center(ui_Label5);
    lv_label_set_text(ui_Label5, language_get_en() ? "Booking room" : "预订会议室");
    lv_obj_set_style_text_color(ui_Label5, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label5, &ui_font_Roboto30, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_BookingCloseBtn = lv_btn_create(ui_BookingDrawer);
    lv_obj_remove_style_all(ui_BookingCloseBtn);
    lv_obj_set_size(ui_BookingCloseBtn, 48, 40);
    lv_obj_align(ui_BookingCloseBtn, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_flag(ui_BookingCloseBtn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_ext_click_area(ui_BookingCloseBtn, 12);
    screen8_style_header_button(ui_BookingCloseBtn);
    lv_obj_add_event_cb(ui_BookingCloseBtn, ui_event_BookingClose, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_BookingCloseBtn, ui_event_BookingClose, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_BookingCloseBtn, ui_event_BookingClose, LV_EVENT_SHORT_CLICKED, NULL);

    ui_BookingCloseLbl = lv_label_create(ui_BookingCloseBtn);
    lv_label_set_text(ui_BookingCloseLbl, "X");
    lv_obj_add_flag(ui_BookingCloseLbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(ui_BookingCloseLbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(ui_BookingCloseLbl);
    lv_obj_set_style_text_color(ui_BookingCloseLbl, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_BookingCloseLbl, &ui_font_Roboto20, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label7 = lv_label_create(ui_BookingDrawer);
    lv_label_set_text(ui_Label7, language_get_en() ? "Time for booking:" : "预订时长:");
    lv_obj_set_style_text_color(ui_Label7, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label7, &ui_font_Roboto30, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Container28 = lv_obj_create(ui_BookingDrawer);
    lv_obj_remove_style_all(ui_Container28);
    lv_obj_set_size(ui_Container28, lv_pct(100), 64);
    lv_obj_set_flex_flow(ui_Container28, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Container28, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Container28, LV_OBJ_FLAG_SCROLLABLE);

    ui_Button2 = lv_btn_create(ui_Container28);
    lv_obj_set_size(ui_Button2, lv_pct(20), lv_pct(75));
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label47 = lv_label_create(ui_Button2);
    lv_obj_center(ui_Label47);
    lv_label_set_text(ui_Label47, language_get_en() ? "15 min" : "15 Min");
    lv_obj_set_style_text_font(ui_Label47, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_Button2, ui_event_DurationBtn, LV_EVENT_CLICKED, (void*)(intptr_t)(15 * 60));

    ui_Button3 = lv_btn_create(ui_Container28);
    lv_obj_set_size(ui_Button3, lv_pct(20), lv_pct(75));
    lv_obj_set_style_radius(ui_Button3, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label48 = lv_label_create(ui_Button3);
    lv_obj_center(ui_Label48);
    lv_label_set_text(ui_Label48, language_get_en() ? "30 min" : "30 Min");
    lv_obj_set_style_text_font(ui_Label48, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_Button3, ui_event_DurationBtn, LV_EVENT_CLICKED, (void*)(intptr_t)(30 * 60));

    ui_Button4 = lv_btn_create(ui_Container28);
    lv_obj_set_size(ui_Button4, lv_pct(20), lv_pct(75));
    lv_obj_set_style_radius(ui_Button4, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label49 = lv_label_create(ui_Button4);
    lv_obj_center(ui_Label49);
    lv_label_set_text(ui_Label49, language_get_en() ? "45 min" : "45 Min");
    lv_obj_set_style_text_font(ui_Label49, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_Button4, ui_event_DurationBtn, LV_EVENT_CLICKED, (void*)(intptr_t)(45 * 60));

    ui_Button5 = lv_btn_create(ui_Container28);
    lv_obj_set_size(ui_Button5, lv_pct(20), lv_pct(75));
    lv_obj_set_style_radius(ui_Button5, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Label50 = lv_label_create(ui_Button5);
    lv_obj_center(ui_Label50);
    lv_label_set_text(ui_Label50, language_get_en() ? "1 hour" : "1 Std");
    lv_obj_set_style_text_font(ui_Label50, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_Button5, ui_event_DurationBtn, LV_EVENT_CLICKED, (void*)(intptr_t)(60 * 60));

    ui_ButtonFullDay = lv_btn_create(ui_Container28);
    lv_obj_set_size(ui_ButtonFullDay, lv_pct(20), lv_pct(75));
    lv_obj_set_style_radius(ui_ButtonFullDay, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_LabelFullDay = lv_label_create(ui_ButtonFullDay);
    lv_obj_center(ui_LabelFullDay);
    lv_label_set_text(ui_LabelFullDay, language_get_en() ? "All day" : "全天");
    lv_obj_set_style_text_font(ui_LabelFullDay, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_ButtonFullDay, ui_event_FullDay, LV_EVENT_CLICKED, NULL);

    ui_ButtonManual = lv_btn_create(ui_BookingDrawer);
    lv_obj_set_size(ui_ButtonManual, lv_pct(92), 46);
    lv_obj_set_style_radius(ui_ButtonManual, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ButtonManual, lv_color_hex(SCREEN8_COLOR_BUTTON), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ButtonManual, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_ButtonManual, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_ButtonManual, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_ButtonManual, lv_color_hex(SCREEN8_COLOR_TEXT_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_LabelManual = lv_label_create(ui_ButtonManual);
    lv_obj_center(ui_LabelManual);
    lv_label_set_text(ui_LabelManual, language_get_en() ? "Manual time" : "自定义时间");
    lv_obj_set_style_text_font(ui_LabelManual, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_ButtonManual, ui_event_ManualOpen, LV_EVENT_CLICKED, NULL);

    ui_Button6 = lv_btn_create(ui_BookingDrawer);
    lv_obj_set_size(ui_Button6, lv_pct(92), 54);
    lv_obj_set_style_radius(ui_Button6, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Button6, lv_color_hex(SCREEN8_COLOR_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button6, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button6, 220, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button6, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui_Button6, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(ui_Button6, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Label51 = lv_label_create(ui_Button6);
    lv_obj_center(ui_Label51);
    lv_label_set_text(ui_Label51, language_get_en() ? "Start now" : "立即开始");
    lv_obj_set_style_text_color(ui_Label51, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label51, &ui_font_Roboto26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_Button6, ui_event_StartNow, LV_EVENT_CLICKED, NULL);

    duration_buttons_refresh();

    const int modal_w = 480;
    const int modal_h = 260;
    ui_ManualPanel = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_ManualPanel);
    lv_obj_set_size(ui_ManualPanel, modal_w, modal_h);
    lv_obj_set_pos(ui_ManualPanel, (screen_w - modal_w) / 2, main_y + ((main_h - modal_h) / 2));
    lv_obj_set_style_pad_all(ui_ManualPanel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_ManualPanel, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_ManualPanel, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(ui_ManualPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_ManualPanel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    screen8_style_surface_card(ui_ManualPanel, lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), 250,
                               lv_color_hex(SCREEN8_COLOR_SURFACE_RAISED), 18);

    lv_obj_t *mp_title = lv_label_create(ui_ManualPanel);
    lv_label_set_text(mp_title, language_get_en() ? "Manual time" : "自定义时间");
    lv_obj_set_style_text_font(mp_title, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(mp_title, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_ManualInner = lv_obj_create(ui_ManualPanel);
    lv_obj_remove_style_all(ui_ManualInner);
    lv_obj_set_size(ui_ManualInner, lv_pct(100), 140);
    lv_obj_set_flex_flow(ui_ManualInner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_ManualInner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_ManualInner, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ui_ManualInner, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_ManualInner, LV_OBJ_FLAG_SCROLLABLE);

    ui_RollerHours = lv_roller_create(ui_ManualInner);
    lv_obj_set_size(ui_RollerHours, 140, 120);
    s_hours_opts = build_options(0, 23);
    if (s_hours_opts) lv_roller_set_options(ui_RollerHours, s_hours_opts, LV_ROLLER_MODE_NORMAL);
    else lv_roller_set_options(ui_RollerHours, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui_RollerHours, 3);
    style_roller_dark(ui_RollerHours);

    lv_obj_t *sp = lv_obj_create(ui_ManualInner);
    lv_obj_set_width(sp, 20);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sp, 0, 0);

    ui_RollerMins = lv_roller_create(ui_ManualInner);
    lv_obj_set_size(ui_RollerMins, 140, 120);
    s_mins_opts = build_options_padded(0, 59);
    if (s_mins_opts) lv_roller_set_options(ui_RollerMins, s_mins_opts, LV_ROLLER_MODE_NORMAL);
    else lv_roller_set_options(ui_RollerMins, "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui_RollerMins, 3);
    style_roller_dark(ui_RollerMins);

    ui_HoursSelFrame = lv_obj_create(lv_obj_get_parent(ui_RollerHours));
    lv_obj_remove_style_all(ui_HoursSelFrame);
    lv_obj_set_size(ui_HoursSelFrame, 140, 44);
    lv_obj_set_style_radius(ui_HoursSelFrame, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_HoursSelFrame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_HoursSelFrame, lv_color_hex(SCREEN8_COLOR_ACCENT_LIGHT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_HoursSelFrame, 85, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_HoursSelFrame, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ui_HoursSelFrame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(ui_HoursSelFrame, ui_RollerHours, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(ui_HoursSelFrame);

    ui_MinsSelFrame = lv_obj_create(lv_obj_get_parent(ui_RollerMins));
    lv_obj_remove_style_all(ui_MinsSelFrame);
    lv_obj_set_size(ui_MinsSelFrame, 140, 44);
    lv_obj_set_style_radius(ui_MinsSelFrame, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_MinsSelFrame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_MinsSelFrame, lv_color_hex(SCREEN8_COLOR_ACCENT_LIGHT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MinsSelFrame, 85, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_MinsSelFrame, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ui_MinsSelFrame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(ui_MinsSelFrame, ui_RollerMins, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(ui_MinsSelFrame);

    lv_obj_t *btn_row = lv_obj_create(ui_ManualPanel);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), 48);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    ui_ManualCancelBtn = lv_btn_create(btn_row);
    lv_obj_set_size(ui_ManualCancelBtn, 160, 40);
    lv_obj_set_style_radius(ui_ManualCancelBtn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ManualCancelBtn, lv_color_hex(SCREEN8_COLOR_DANGER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ManualCancelBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ManualCancelBtn, lv_color_hex(SCREEN8_COLOR_DANGER_LIGHT), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ui_ManualCancelBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_ManualCancelBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_ManualCancelBtn, ui_event_ManualCancel, LV_EVENT_CLICKED, NULL);
    ui_ManualCancelLbl = lv_label_create(ui_ManualCancelBtn);
    lv_label_set_text(ui_ManualCancelLbl, language_get_en() ? "Cancel" : "取消");
    lv_obj_set_style_text_color(ui_ManualCancelLbl, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ManualCancelLbl, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ui_ManualCancelLbl);

    ui_ManualConfirmBtn = lv_btn_create(btn_row);
    lv_obj_set_size(ui_ManualConfirmBtn, 160, 40);
    lv_obj_set_style_radius(ui_ManualConfirmBtn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ManualConfirmBtn, lv_color_hex(SCREEN8_COLOR_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ManualConfirmBtn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ManualConfirmBtn, lv_color_hex(SCREEN8_COLOR_ACCENT_LIGHT), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ui_ManualConfirmBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_ManualConfirmBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_ManualConfirmBtn, ui_event_ManualConfirm, LV_EVENT_CLICKED, NULL);
    ui_ManualConfirmLbl = lv_label_create(ui_ManualConfirmBtn);
    lv_label_set_text(ui_ManualConfirmLbl, language_get_en() ? "OK" : "确定");
    lv_obj_set_style_text_color(ui_ManualConfirmLbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ManualConfirmLbl, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ui_ManualConfirmLbl);

    ui_Container3 = lv_obj_create(ui_Screen8);
    lv_obj_remove_style_all(ui_Container3);
    lv_obj_set_size(ui_Container3, main_w, SCREEN8_FOOTER_H);
    lv_obj_set_flex_flow(ui_Container3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Container3, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(ui_Container3, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui_Container3, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui_Container3, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_Container3, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ui_Container3, SCREEN8_MAIN_GAP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Container3, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    screen8_style_surface_card(ui_Container3, lv_color_hex(SCREEN8_COLOR_SURFACE_ALT), 245,
                               lv_color_hex(SCREEN8_COLOR_SURFACE_ALT), 18);

    ui_Panel2 = lv_obj_create(ui_Container3);
    lv_obj_set_size(ui_Panel2, footer_item_w, 36);
    lv_obj_set_flex_flow(ui_Panel2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Panel2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_Panel2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Panel2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_Panel2, LV_OBJ_FLAG_CLICKABLE);
    screen8_style_nav_item(ui_Panel2);

    ui_Image12 = lv_img_create(ui_Panel2);
    lv_img_set_src(ui_Image12, &ui_img_infoicon_png);
    lv_obj_add_flag(ui_Image12, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_Image12, LV_OBJ_FLAG_SCROLLABLE);

    ui_Label38 = lv_label_create(ui_Panel2);
    lv_label_set_text(ui_Label38, language_get_en() ? "Info" : "信息");
    lv_obj_set_style_text_color(ui_Label38, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label38, &ui_font_Roboto26, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Panel20 = lv_obj_create(ui_Container3);
    lv_obj_set_size(ui_Panel20, footer_item_w, 36);
    lv_obj_set_flex_flow(ui_Panel20, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Panel20, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_Panel20, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_Panel20, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_Panel20, LV_OBJ_FLAG_CLICKABLE);
    screen8_style_nav_item(ui_Panel20);

    ui_Image2 = lv_img_create(ui_Panel20);
    lv_img_set_src(ui_Image2, &ui_img_630012529);
    lv_obj_add_flag(ui_Image2, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_Image2, LV_OBJ_FLAG_SCROLLABLE);

    ui_Label45 = lv_label_create(ui_Panel20);
    lv_label_set_text(ui_Label45, language_get_en() ? "Weather" : "天气");
    lv_obj_set_style_text_color(ui_Label45, lv_color_hex(SCREEN8_COLOR_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label45, &ui_font_Roboto26, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_ImgBtn, ui_event_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Panel2, ui_event_Panel2, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Panel4, ui_event_Panel4, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Panel20, ui_event_BottomWeather, LV_EVENT_CLICKED, NULL);

    s_booking_open = false;
    booking_recalc_positions();
    if (ui_BookingDrawer) lv_obj_set_x(ui_BookingDrawer, (lv_coord_t)s_closed_x);

    restore_booking_state_ui();
    ui_Screen8_refresh_device_name();
    s_lang_cb_id = language_register_callback(ui_language_changed_cb, NULL);
}

/* ===================== Screen destroy ===================== */

void ui_Screen8_screen_destroy(void)
{
    if (s_stop_confirm_msgbox) {
        lv_obj_del(s_stop_confirm_msgbox);
        s_stop_confirm_msgbox = NULL;
    }

    /* stop UI countdown updates, but keep active booking state so it can be restored later */
    countdown_delete_timer();

    /* stop clock task FIRST */
    screen8_clock_stop();

    /* null time label pointers early (protects async calls) */
    ui_Label41 = NULL;
    ui_Label42 = NULL;

    /* remove one-min blink timer if any */
    if (s_one_min_blink_timer) {
        lv_timer_del(s_one_min_blink_timer);
        s_one_min_blink_timer = NULL;
    }
    s_one_min_active = false;


    if (s_wifi_icon_timer) {
        lv_timer_del(s_wifi_icon_timer);
        s_wifi_icon_timer = NULL;
    }

    /* unregister language callback for this screen */
    if (s_lang_cb_id > 0) {
        language_unregister_callback(s_lang_cb_id);
        s_lang_cb_id = -1;
    }

    weather_integration_destroy_widget();
    s_weather_widget = NULL;

    if (ui_Screen8) lv_obj_del(ui_Screen8);

    /* free heap buffers for rollers to avoid leak and keep LVGL stable */
    if (s_hours_opts) { free(s_hours_opts); s_hours_opts = NULL; }
    if (s_mins_opts)  { free(s_mins_opts);  s_mins_opts = NULL; }

    ui_Screen8 = NULL;

    ui_Container2 = NULL;
    ui_Label4 = NULL;
    ui_Image4 = NULL;

    ui_Container4 = NULL;
    ui_Container8 = NULL;

    ui_Container26 = NULL;

    ui_Container27 = NULL;

    ui_Panel4 = NULL;
    ui_Label44 = NULL;
    ui_TimerLabel = NULL;

    ui_Container25 = NULL;
    s_weather_widget = NULL;

    ui_BookingOverlay = NULL;
    ui_BookingDrawer = NULL;
    ui_BookingCloseBtn = NULL;
    ui_BookingCloseLbl = NULL;
    ui_BookingHeader = NULL;

    ui_Label5 = NULL;
    ui_Label7 = NULL;
    ui_Container28 = NULL;

    ui_Button2 = NULL;
    ui_Label47 = NULL;
    ui_Button3 = NULL;
    ui_Label48 = NULL;
    ui_Button4 = NULL;
    ui_Label49 = NULL;
    ui_Button5 = NULL;
    ui_Label50 = NULL;

    ui_ButtonFullDay = NULL;
    ui_LabelFullDay = NULL;

    ui_ButtonManual = NULL;
    ui_LabelManual = NULL;

    ui_Button6 = NULL;
    ui_Label51 = NULL;

    ui_ManualPanel = NULL;
    ui_ManualInner = NULL;
    ui_RollerHours = NULL;
    ui_RollerMins = NULL;
    ui_ManualConfirmBtn = NULL;
    ui_ManualCancelBtn = NULL;

    ui_ManualCancelLbl = NULL;
    ui_ManualConfirmLbl = NULL;

    ui_HoursSelFrame = NULL;
    ui_MinsSelFrame  = NULL;

    ui_Container3 = NULL;
    ui_Panel2 = NULL;
    ui_Image12 = NULL;
    ui_Label38 = NULL;

    ui_Panel1 = NULL;
    ui_Image1 = NULL;
    ui_Label6 = NULL;

    ui_Panel20 = NULL;
    ui_Image2 = NULL;
    ui_Label45 = NULL;
    ui_ImgBtn = NULL;
    ui_ImgBtnImg = NULL;
    ui_WifiIcon = NULL;

    /* language UI pointers */
    ui_LangSwitch = NULL;
    ui_LangLabel = NULL;
    s_lang_label_y_orig = LV_COORD_MIN;

    s_booking_open = false;
    s_stop_confirm_msgbox = NULL;
}
