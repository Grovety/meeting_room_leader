#include "weather_widget.h"
#include "weather_icons.h"
#include "weather_ui.h"
#include "weather_integration.h"
#include "language_manager.h"
#include <esp_log.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern "C" {
extern const lv_font_t ui_font_Roboto50;
extern const lv_font_t ui_font_Roboto40;
extern const lv_font_t ui_font_FontZh30;
extern const lv_font_t ui_font_FontZh26;
extern const lv_font_t ui_font_FontZh20;
extern const lv_font_t ui_font_FontZh18;
extern const lv_font_t ui_font_FontZh16;
}

#define ui_font_Roboto30 ui_font_FontZh30
#define ui_font_Roboto26 ui_font_FontZh26
#define ui_font_Roboto20 ui_font_FontZh20
#define ui_font_Roboto18 ui_font_FontZh18
#define ui_font_Roboto16 ui_font_FontZh16

struct weather_widget_t {
    lv_obj_t* container;
    lv_obj_t* main_row;
    lv_obj_t* info;
    lv_obj_t* icon;
    lv_obj_t* temp_label;
    lv_obj_t* desc_label;
    lv_obj_t* city_label;
    lv_obj_t* feels_title_label;
    lv_obj_t* feels_like_label;
    lv_obj_t* precip_card;
    lv_obj_t* precip_title_label;
    lv_obj_t* precipitation_label;
    lv_obj_t* wind_label;
    lv_obj_t* empty_state_label;
    void (*click_callback)(void*);
    void* user_data;
};

static const lv_color_t kWeatherWidgetSurfaceColor = LV_COLOR_MAKE(0x2A, 0x25, 0x22);
static const lv_color_t kWeatherWidgetCardColor = LV_COLOR_MAKE(0x34, 0x2D, 0x29);
static const lv_color_t kWeatherWidgetCardBorderColor = LV_COLOR_MAKE(0x4A, 0x42, 0x3D);
static const lv_color_t kWeatherWidgetTextPrimary = LV_COLOR_MAKE(0xF5, 0xEF, 0xE8);
static const lv_color_t kWeatherWidgetTextSecondary = LV_COLOR_MAKE(0xB9, 0xAD, 0xA2);
static const lv_color_t kWeatherWidgetTextMuted = LV_COLOR_MAKE(0xD6, 0xCB, 0xC1);
static const lv_color_t kWeatherWidgetDryCardColor = LV_COLOR_MAKE(0x6E, 0x95, 0x5F);
static const lv_color_t kWeatherWidgetDryBorderColor = LV_COLOR_MAKE(0x93, 0xB7, 0x83);

static const char* weather_widget_text(bool en, const char* en_text, const char* zh_text) {
    return en ? en_text : zh_text;
}

static const char* weather_widget_wind_unit(void) {
    return weather_integration_get_metric_units() ? "m/s" : "mph";
}

static const char* weather_widget_temp_unit(void) {
    return weather_integration_get_metric_units() ? "C" : "F";
}

static const char* weather_widget_status_message(weather_data_status_t status) {
    switch (status) {
        case WEATHER_DATA_STATUS_API_KEY_REQUIRED:
            return "API key required";
        case WEATHER_DATA_STATUS_INVALID_API_KEY:
            return "Invalid API key";
        default:
            return NULL;
    }
}

static void format_precipitation_time(uint32_t next_time, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (next_time == 0) {
        return;
    }

    time_t precip_time = (time_t)next_time;

    struct tm precip_tm;
    localtime_r(&precip_time, &precip_tm);

    strftime(buffer, buffer_size, "%H:%M", &precip_tm);
}

static const char* get_wind_direction_str(float degrees) {
    const bool en = language_get_en();

    if (degrees >= 337.5 || degrees < 22.5) return "N";
    if (degrees >= 22.5 && degrees < 67.5) return en ? "NE" : "NO";
    if (degrees >= 67.5 && degrees < 112.5) return en ? "E" : "O";
    if (degrees >= 112.5 && degrees < 157.5) return en ? "SE" : "SO";
    if (degrees >= 157.5 && degrees < 202.5) return "S";
    if (degrees >= 202.5 && degrees < 247.5) return "SW";
    if (degrees >= 247.5 && degrees < 292.5) return "W";
    if (degrees >= 292.5 && degrees < 337.5) return en ? "NW" : "NW";
    return "-";
}

static void widget_click_handler(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        weather_widget_t* widget = (weather_widget_t*)lv_event_get_user_data(e);
        if (widget && widget->click_callback) {
            widget->click_callback(widget->user_data);
        }
    }
}

static void make_clickable(lv_obj_t* obj, weather_widget_t* widget) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, widget_click_handler, LV_EVENT_CLICKED, widget);
}

static void make_bubbling_click_target(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* create_info_card(lv_obj_t* parent, int32_t width, lv_obj_t** title_label, lv_obj_t** value_label,
        const lv_font_t* value_font) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, width, 78);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_color(card, kWeatherWidgetCardColor, 0);
    lv_obj_set_style_bg_opa(card, 245, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kWeatherWidgetCardBorderColor, 0);
    lv_obj_set_style_pad_left(card, 8, 0);
    lv_obj_set_style_pad_right(card, 8, 0);
    lv_obj_set_style_pad_top(card, 5, 0);
    lv_obj_set_style_pad_bottom(card, 5, 0);
    lv_obj_set_style_pad_gap(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_shadow_opa(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_bubbling_click_target(card);

    *title_label = lv_label_create(card);
    lv_obj_set_width(*title_label, width - 12);
    lv_label_set_long_mode(*title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(*title_label, &ui_font_Roboto18, 0);
    lv_obj_set_style_text_color(*title_label, kWeatherWidgetTextSecondary, 0);
    lv_obj_set_style_text_letter_space(*title_label, 0, 0);
    lv_obj_set_style_text_align(*title_label, LV_TEXT_ALIGN_CENTER, 0);
    make_bubbling_click_target(*title_label);

    *value_label = lv_label_create(card);
    lv_obj_set_width(*value_label, width - 18);
    lv_label_set_long_mode(*value_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(*value_label, value_font, 0);
    lv_obj_set_style_text_color(*value_label, kWeatherWidgetTextPrimary, 0);
    lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_CENTER, 0);
    make_bubbling_click_target(*value_label);

    return card;
}

static void format_signed_temp(char* buffer, size_t size, float value) {
    if (value >= 0) snprintf(buffer, size, "+%.0f°%s", (double)value, weather_widget_temp_unit());
    else snprintf(buffer, size, "%.0f°%s", (double)value, weather_widget_temp_unit());
}

static void capitalize_first_letter(char* text) {
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] -= 32;
    }
}

static void set_precipitation_summary(weather_widget_t* widget, const weather_data_t* data) {
    const bool en = language_get_en();
    char compact_title[32];
    char value[32];
    bool compact_value_layout = false;
    bool has_today_precip = false;
    float chance = 0.0f;
    float amount = 0.0f;
    uint32_t next_time = 0;
    weather_today_outlook_t today_outlook;
    memset(&today_outlook, 0, sizeof(today_outlook));
    bool has_today_outlook = weather_get_today_outlook(&today_outlook);

    if (has_today_outlook &&
        (today_outlook.pop > 0.05f || today_outlook.rain > 0.0f || today_outlook.snow > 0.0f)) {
        has_today_precip = true;
        chance = today_outlook.pop;
        amount = today_outlook.snow > today_outlook.rain ? today_outlook.snow : today_outlook.rain;
        next_time = today_outlook.precipitation_time;
    }

    lv_color_t card_color = kWeatherWidgetCardColor;
    lv_color_t border_color = kWeatherWidgetCardBorderColor;
    lv_color_t title_color = kWeatherWidgetTextSecondary;
    lv_color_t value_color = kWeatherWidgetTextPrimary;
    const char* title_text = weather_widget_text(en, "OUTLOOK", "天气概况");
    const char* type_text = weather_widget_text(en, "Rain", "雨");
    compact_title[0] = '\0';

    if (!has_today_outlook && data->timestamp == 0) {
        snprintf(value, sizeof(value), "%s", weather_widget_text(en, "Loading...", "正在加载..."));
    } else if (has_today_precip) {
        card_color = LV_COLOR_MAKE(0x23, 0x3F, 0x68);
        border_color = LV_COLOR_MAKE(0x6A, 0xB7, 0xFF);
        if ((today_outlook.snow > today_outlook.rain && today_outlook.snow > 0.0f) ||
            strcmp(today_outlook.precipitation_type, "snow") == 0) {
            type_text = weather_widget_text(en, "Snow", "雪");
        } else if (strcmp(today_outlook.precipitation_type, "mixed") == 0) {
            type_text = weather_widget_text(en, "Mixed", "雨雪");
        }

        if (chance > 0.0f) {
            if (next_time > 0) {
                char time_buf[24];
                format_precipitation_time(next_time, time_buf, sizeof(time_buf));
                snprintf(compact_title, sizeof(compact_title), "%s %.0f%%", type_text, (double)(chance * 100.0f));
                title_text = compact_title;
                snprintf(value, sizeof(value), "%s", time_buf);
                compact_value_layout = true;
            } else {
                snprintf(value, sizeof(value), "%s %.0f%%", type_text, (double)(chance * 100.0f));
            }
        } else if (amount > 0.0f) {
            snprintf(value, sizeof(value), "%s %.1f mm", type_text, (double)amount);
        } else {
            snprintf(value, sizeof(value), "%s", type_text);
        }
    } else if (has_today_outlook) {
        card_color = kWeatherWidgetDryCardColor;
        border_color = kWeatherWidgetDryBorderColor;
        snprintf(value, sizeof(value), "%s", weather_widget_text(en, "Dry today", "今日暂无降水"));
    } else if (data->pop > 0.05f || data->rain > 0.0f || data->snow > 0.0f) {
        card_color = LV_COLOR_MAKE(0x23, 0x3F, 0x68);
        border_color = LV_COLOR_MAKE(0x6A, 0xB7, 0xFF);
        if (data->snow > data->rain && data->snow > 0.0f) {
            type_text = weather_widget_text(en, "Snow", "雪");
        }
        chance = data->pop;
        amount = data->snow > data->rain ? data->snow : data->rain;
        if (chance > 0.0f) {
            snprintf(value, sizeof(value), "%s %.0f%%", type_text, (double)(chance * 100.0f));
        } else if (amount > 0.0f) {
            snprintf(value, sizeof(value), "%s %.1f mm", type_text, (double)amount);
        } else {
            snprintf(value, sizeof(value), "%s", type_text);
        }
    } else {
        snprintf(value, sizeof(value), "%s", weather_widget_text(en, "Loading...", "正在加载..."));
    }

    lv_label_set_text(widget->precip_title_label, title_text);
    lv_label_set_text(widget->precipitation_label, value);
    lv_obj_set_style_bg_color(widget->precip_card, card_color, 0);
    lv_obj_set_style_border_color(widget->precip_card, border_color, 0);
    lv_obj_set_style_text_color(widget->precip_title_label, compact_value_layout ? value_color : title_color, 0);
    lv_obj_set_style_text_color(widget->precipitation_label, value_color, 0);
    lv_obj_set_style_pad_gap(widget->precip_card, 0, 0);
    lv_obj_set_style_text_font(widget->precip_title_label,
                               compact_value_layout ? &ui_font_Roboto26 : &ui_font_Roboto16,
                               0);
    lv_obj_set_style_text_font(widget->precipitation_label,
                               compact_value_layout ? &ui_font_Roboto26 : &ui_font_Roboto26,
                               0);
}

weather_widget_t* weather_widget_create(lv_obj_t* parent) {
    if (!parent) return NULL;
    
    weather_widget_t* widget = (weather_widget_t*)malloc(sizeof(weather_widget_t));
    if (!widget) return NULL;
    memset(widget, 0, sizeof(weather_widget_t));
    
    widget->container = lv_obj_create(parent);
    lv_obj_set_size(widget->container, 390, 350);
    lv_obj_set_style_clip_corner(widget->container, true, 0);
    lv_obj_set_style_bg_color(widget->container, kWeatherWidgetSurfaceColor, 0);
    lv_obj_set_style_bg_opa(widget->container, 255, 0);
    lv_obj_set_style_radius(widget->container, 30, 0);
    lv_obj_set_style_border_width(widget->container, 0, 0);
    lv_obj_set_style_pad_all(widget->container, 18, 0);
    lv_obj_set_style_shadow_width(widget->container, 22, 0);
    lv_obj_set_style_shadow_opa(widget->container, 30, 0);
    lv_obj_set_style_shadow_color(widget->container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_ofs_y(widget->container, 10, 0);
    lv_obj_set_flex_flow(widget->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(widget->container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_clickable(widget->container, widget);

    widget->main_row = lv_obj_create(widget->container);
    lv_obj_set_size(widget->main_row, lv_pct(100), 178);
    lv_obj_set_style_bg_opa(widget->main_row, 0, 0);
    lv_obj_set_style_border_width(widget->main_row, 0, 0);
    lv_obj_set_flex_flow(widget->main_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->main_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(widget->main_row, 0, 0);
    lv_obj_set_style_pad_gap(widget->main_row, 12, 0);
    make_bubbling_click_target(widget->main_row);

    widget->icon = lv_img_create(widget->main_row);
    lv_img_set_zoom(widget->icon, 360);
    lv_obj_set_width(widget->icon, 110);
    make_bubbling_click_target(widget->icon);

    lv_obj_t* text_col = lv_obj_create(widget->main_row);
    lv_obj_set_size(text_col, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(text_col, 0, 0);
    lv_obj_set_style_border_width(text_col, 0, 0);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(text_col, 5, 0);
    make_bubbling_click_target(text_col);

    widget->city_label = lv_label_create(text_col);
    lv_obj_set_width(widget->city_label, 220);
    lv_label_set_long_mode(widget->city_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(widget->city_label, &ui_font_Roboto26, 0);
    lv_obj_set_style_text_color(widget->city_label, kWeatherWidgetTextSecondary, 0);
    lv_obj_set_style_text_align(widget->city_label, LV_TEXT_ALIGN_LEFT, 0);
    make_bubbling_click_target(widget->city_label);

    widget->temp_label = lv_label_create(text_col);
    lv_obj_set_width(widget->temp_label, 220);
    lv_obj_set_style_text_font(widget->temp_label, &ui_font_Roboto50, 0);
    lv_obj_set_style_text_color(widget->temp_label, kWeatherWidgetTextPrimary, 0);
    lv_obj_set_style_text_letter_space(widget->temp_label, 1, 0);
    lv_obj_set_style_text_align(widget->temp_label, LV_TEXT_ALIGN_LEFT, 0);
    make_bubbling_click_target(widget->temp_label);

    widget->desc_label = lv_label_create(text_col);
    /* Extra-safe layout for long weather descriptions on the home widget. */
    lv_obj_set_width(widget->desc_label, 198);
    lv_label_set_long_mode(widget->desc_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(widget->desc_label, &ui_font_Roboto30, 0);
    lv_obj_set_style_text_color(widget->desc_label, kWeatherWidgetTextMuted, 0);
    lv_obj_set_style_text_align(widget->desc_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_letter_space(widget->desc_label, 0, 0);
    make_bubbling_click_target(widget->desc_label);

    widget->info = lv_obj_create(widget->container);
    lv_obj_set_size(widget->info, lv_pct(100), 132);
    lv_obj_set_style_clip_corner(widget->info, true, 0);
    lv_obj_set_style_bg_color(widget->info, kWeatherWidgetSurfaceColor, 0);
    lv_obj_set_style_bg_opa(widget->info, 255, 0);
    lv_obj_set_style_radius(widget->info, 24, 0);
    lv_obj_set_style_border_width(widget->info, 0, 0);
    lv_obj_set_style_shadow_width(widget->info, 0, 0);
    lv_obj_set_style_shadow_opa(widget->info, 0, 0);
    lv_obj_set_style_pad_left(widget->info, 12, 0);
    lv_obj_set_style_pad_right(widget->info, 12, 0);
    lv_obj_set_style_pad_top(widget->info, 12, 0);
    lv_obj_set_style_pad_bottom(widget->info, 10, 0);
    lv_obj_set_style_pad_gap(widget->info, 4, 0);
    lv_obj_set_flex_flow(widget->info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(widget->info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_bubbling_click_target(widget->info);

    lv_obj_t* cards_row = lv_obj_create(widget->info);
    lv_obj_set_size(cards_row, lv_pct(100), 78);
    lv_obj_set_style_bg_opa(cards_row, 0, 0);
    lv_obj_set_style_border_width(cards_row, 0, 0);
    lv_obj_set_style_pad_all(cards_row, 0, 0);
    lv_obj_set_style_pad_gap(cards_row, 10, 0);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_bubbling_click_target(cards_row);

    create_info_card(cards_row, 154, &widget->feels_title_label, &widget->feels_like_label, &ui_font_Roboto40);
    lv_label_set_text(widget->feels_title_label, language_get_en() ? "FEELS LIKE" : "体感温度");

    widget->precip_card = create_info_card(cards_row, 154, &widget->precip_title_label, &widget->precipitation_label,
            &ui_font_Roboto30);
    lv_label_set_text(widget->precip_title_label, language_get_en() ? "OUTLOOK" : "天气概况");
    lv_obj_set_style_text_font(widget->precip_title_label, &ui_font_Roboto16, 0);
    lv_obj_set_style_text_font(widget->precipitation_label, &ui_font_Roboto26, 0);

    widget->wind_label = lv_label_create(widget->info);
    lv_obj_set_width(widget->wind_label, lv_pct(100));
    lv_label_set_long_mode(widget->wind_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(widget->wind_label, &ui_font_Roboto26, 0);
    lv_obj_set_style_text_color(widget->wind_label, kWeatherWidgetTextSecondary, 0);
    lv_obj_set_style_text_align(widget->wind_label, LV_TEXT_ALIGN_CENTER, 0);
    make_bubbling_click_target(widget->wind_label);

    widget->empty_state_label = lv_label_create(widget->container);
    lv_obj_set_width(widget->empty_state_label, 320);
    lv_label_set_long_mode(widget->empty_state_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(widget->empty_state_label, &ui_font_Roboto30, 0);
    lv_obj_set_style_text_color(widget->empty_state_label, kWeatherWidgetTextPrimary, 0);
    lv_obj_set_style_text_align(widget->empty_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(widget->empty_state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(widget->empty_state_label);
    make_bubbling_click_target(widget->empty_state_label);

    return widget;
}

void weather_widget_update(weather_widget_t* widget, const weather_data_t* data) {
    if (!widget || !data || !widget->container || !lv_obj_is_valid(widget->container)) return;
    char buf[128];

    const weather_data_status_t status = weather_module_get_status();
    const char* status_message = weather_widget_status_message(status);
    if (status_message) {
        lv_obj_clear_flag(widget->main_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(widget->info, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(widget->empty_state_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(widget->city_label, "");
        lv_label_set_text(widget->temp_label, weather_integration_get_metric_units() ? "--°C" : "--°F");
        lv_label_set_text(widget->desc_label, status_message);
        weather_ui_set_icon(widget->icon, "01d");
        lv_label_set_text(widget->feels_title_label, language_get_en() ? "FEELS LIKE" : "体感温度");
        lv_label_set_text(widget->feels_like_label, "");
        lv_label_set_text(widget->precip_title_label, language_get_en() ? "OUTLOOK" : "天气概况");
        lv_label_set_text(widget->precipitation_label, "");
        lv_obj_set_style_bg_color(widget->precip_card, kWeatherWidgetCardColor, 0);
        lv_obj_set_style_border_color(widget->precip_card, kWeatherWidgetCardBorderColor, 0);
        lv_obj_set_style_text_color(widget->precip_title_label, kWeatherWidgetTextSecondary, 0);
        lv_obj_set_style_text_color(widget->precipitation_label, kWeatherWidgetTextPrimary, 0);
        lv_obj_set_style_text_font(widget->precip_title_label, &ui_font_Roboto16, 0);
        lv_obj_set_style_text_font(widget->precipitation_label, &ui_font_Roboto26, 0);
        lv_obj_set_style_pad_gap(widget->precip_card, 0, 0);
        lv_label_set_text(widget->wind_label, "");
        return;
    }

    lv_obj_clear_flag(widget->main_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(widget->info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(widget->empty_state_label, LV_OBJ_FLAG_HIDDEN);
    
    lv_label_set_text(widget->city_label, data->city[0] != '\0' ? data->city : "---");

    format_signed_temp(buf, sizeof(buf), data->temperature);
    lv_label_set_text(widget->temp_label, buf);

    if (data->description[0] != '\0') {
        strncpy(buf, data->description, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        capitalize_first_letter(buf);
        lv_label_set_text(widget->desc_label, buf);
    } else {
        lv_label_set_text(widget->desc_label, language_get_en() ? "No data" : "暂无数据");
    }

    weather_ui_set_icon(widget->icon, data->icon);

    format_signed_temp(buf, sizeof(buf), data->feels_like);
    lv_label_set_text(widget->feels_like_label, buf);
    set_precipitation_summary(widget, data);

    const char* d = get_wind_direction_str(data->wind_direction);
    if (data->wind_speed < 0.8f) {
        snprintf(buf, sizeof(buf), "%s", language_get_en() ? "Wind calm" : "风速弱");
    } else if (strcmp(d, "-") == 0) {
        snprintf(buf, sizeof(buf), language_get_en() ? "Wind %.0f %s" : "风速 %.0f %s",
                 (double)data->wind_speed, weather_widget_wind_unit());
    } else {
        snprintf(buf, sizeof(buf), language_get_en() ? "Wind %s %.0f %s" : "风速 %s %.0f %s",
                 d, (double)data->wind_speed, weather_widget_wind_unit());
    }
    lv_label_set_text(widget->wind_label, buf);
}

void weather_widget_refresh_language(weather_widget_t* widget) {
    if (!widget || !widget->container || !lv_obj_is_valid(widget->container)) return;

    lv_label_set_text(widget->feels_title_label, language_get_en() ? "FEELS LIKE" : "体感温度");

    weather_data_t data;
    memset(&data, 0, sizeof(data));

    if (weather_get_current_data(&data)) {
        weather_widget_update(widget, &data);
        return;
    }

    strncpy(data.city, language_get_en() ? "Loading..." : "正在加载...", sizeof(data.city) - 1);
    strncpy(data.description, language_get_en() ? "Waiting for data..." : "等待数据...", sizeof(data.description) - 1);
    strncpy(data.icon, "01d", sizeof(data.icon) - 1);
    weather_widget_update(widget, &data);
}

void weather_widget_set_pos(weather_widget_t* widget, int32_t x, int32_t y) {
    if (widget) lv_obj_set_pos(widget->container, x, y);
}

void weather_widget_set_size(weather_widget_t* widget, int32_t width, int32_t height) {
    if (widget) lv_obj_set_size(widget->container, width, height);
}

void weather_widget_set_align(weather_widget_t* widget, lv_align_t align, int32_t x_ofs, int32_t y_ofs) {
    if (widget) lv_obj_align(widget->container, align, x_ofs, y_ofs);
}

lv_obj_t* weather_widget_get_obj(weather_widget_t* widget) {
    return widget ? widget->container : NULL;
}

void weather_widget_set_click_callback(weather_widget_t* widget, void (*callback)(void*), void* user_data) {
    if (widget) {
        widget->click_callback = callback;
        widget->user_data = user_data;
    }
}

void weather_widget_delete(weather_widget_t* widget) {
    if (widget) {
        if (widget->container) lv_obj_del(widget->container);
        free(widget);
    }
}
