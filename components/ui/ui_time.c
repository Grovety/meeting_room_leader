// ui_time.c
#include "ui.h"
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "language_manager.h"  // <-- localization

static const char *TAG = "ui_time";

/* Initialize default values */
uint8_t ui_time_hour = 10;
uint8_t ui_time_minute = 20;

/* Local prototypes */
static void save_time_to_nvs(void);
static void ui_update_time_widgets(uint8_t hour, uint8_t minute, bool save);

/* Exported date update function */
void ui_update_date_display(int year, int month, int day, int wday);

/* Store last date for updates on language change */
static int s_last_year = -1;
static int s_last_month = -1;
static int s_last_day = -1;
static int s_last_wday = -1;

/* Registered language callback id (if >0, registered) */
static int s_lang_cb_id = -1;

/* Language-change callback: update date label in selected language */
static void ui_time_lang_cb(bool en, void *ctx)
{
    (void)en;
    (void)ctx;
    if (s_last_wday >= 0 && s_last_month >= 1 && s_last_month <= 12 && s_last_day > 0) {
        ui_update_date_display(s_last_year, s_last_month, s_last_day, s_last_wday);
    } else {
        /* If no date yet, use current local time */
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);
        ui_update_date_display(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday);
    }
}

/* Time initialization */
void ui_time_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ui_time", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS is not initialized yet, using default UI time");
    }
    if (err == ESP_OK) {
        uint8_t hval = 0, mval = 0;
        esp_err_t e1 = nvs_get_u8(h, "hour", &hval);
        esp_err_t e2 = nvs_get_u8(h, "minute", &mval);
        nvs_close(h);

        if (e1 == ESP_OK && e2 == ESP_OK) {
            ui_time_hour = hval;
            ui_time_minute = mval;
            ESP_LOGI(TAG, "Loaded time %02u:%02u from NVS", ui_time_hour, ui_time_minute);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "Unable to read stored UI time: %s", esp_err_to_name(err));
    }

    ui_set_time(ui_time_hour, ui_time_minute);

    /* Register language callback; callback is invoked immediately on registration */
    s_lang_cb_id = language_register_callback(ui_time_lang_cb, NULL);
}

/* Save time to NVS */
static void save_time_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ui_time", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %d", err);
        return;
    }

    nvs_set_u8(h, "hour", ui_time_hour);
    nvs_set_u8(h, "minute", ui_time_minute);

    err = nvs_commit(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved time %02u:%02u to NVS", ui_time_hour, ui_time_minute);
    }

    nvs_close(h);
}

/* Common time update logic */
static void ui_update_time_widgets(uint8_t hour, uint8_t minute, bool save)
{
    ui_time_hour = hour;
    ui_time_minute = minute;

    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", hour, minute);

    if (ui_Label41) lv_label_set_text(ui_Label41, buf);

    if (save) save_time_to_nvs();
}

/* Set time with persistence */
void ui_set_time(uint8_t hour, uint8_t minute)
{
    ui_update_time_widgets(hour, minute, true);
}

/* Update time without persistence */
void ui_update_time_display(uint8_t hour, uint8_t minute)
{
    ui_update_time_widgets(hour, minute, false);
}

/*
 * Date update
 * FORMAT MUST MATCH WeatherUI:
 * "Monday, 28 Dec"
 *
 * Supports EN/ZH (uses language_get_en()).
 * Stores the provided date so it can be recalculated on language change.
 */
void ui_update_date_display(int year, int month, int day, int wday)
{
    /* Save last date for later update on language change */
    s_last_year = year;
    s_last_month = month;
    s_last_day = day;
    s_last_wday = wday;

    /* English/Chinese */
    static const char *days_en[] = {
        "Sunday", "Monday", "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday"
    };

    static const char *months_en[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    static const char *days_de[] = {
        "星期日", "星期一", "星期二",
        "星期三", "星期四", "星期五", "星期六"
    };

    static const char *months_de[] = {
        "1月", "2月", "3月", "4月", "5月", "6月",
        "7月", "8月", "9月", "10月", "11月", "12月"
    };

    char buf[32];

    bool en = language_get_en();

    if (month >= 1 && month <= 12 && wday >= 0 && wday <= 6) {
        const char *day_str = en ? days_en[wday] : days_de[wday];
        const char *month_str = en ? months_en[month - 1] : months_de[month - 1];

        snprintf(buf, sizeof(buf),
                 "%s, %d %s",
                 day_str,
                 day,
                 month_str);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }

    if (ui_Label42) lv_label_set_text(ui_Label42, buf);
}
