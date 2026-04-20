#include "weather_ui.h"
#include <esp_log.h>
#include <string.h>
#include "lvgl.h"
#include <cstdio>
#include <ctype.h> 
#include <time.h>
#include "weather_icons.h"
#include "language_manager.h"
#include "weather_integration.h"

static const char* TAG = "WeatherUI";

extern "C" {
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock(void);
extern const lv_font_t ui_font_Fonttt16;
extern const lv_font_t ui_font_FontZh14;
extern const lv_font_t ui_font_FontZh16;
extern const lv_font_t ui_font_FontZh18;
extern const lv_font_t ui_font_FontZh20;
extern const lv_font_t ui_font_FontZh26;
}

#define COLOR_BG          lv_color_hex(0x12100F)
#define COLOR_CARD        lv_color_hex(0x303031)    
#define COLOR_CARD_BORDER lv_color_hex(0x3D3D3D)   
#define COLOR_TEXT_MAIN   lv_color_hex(0xFFFFFF)    
#define COLOR_TEXT_DIM    lv_color_hex(0xCCCCCC)    
#define COLOR_ACCENT      lv_color_hex(0x38BDF8)    
#define COLOR_SECONDARY   lv_color_hex(0x94A3B8)   
#define COLOR_SUNSET      lv_color_hex(0xfb923c)    
#define COLOR_NIGHT_TEMP  lv_color_hex(0x0674a4)  
#define COLOR_BUTTON      lv_color_hex(0x303031)  
#define COLOR_PRECIP      lv_color_hex(0x4F8EF7) 

#define DISABLE_SCROLL(obj) if(obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE)

static const char* weather_ui_text(const char* en, const char* zh)
{
    return language_get_en() ? en : zh;
}

static bool weather_ui_is_metric_units()
{
    return weather_integration_get_metric_units();
}

static void weather_ui_format_temperature(float value, char* buffer, size_t buffer_size, bool with_unit)
{
    const char* suffix = with_unit ? (weather_ui_is_metric_units() ? "°C" : "°F") : "°";
    snprintf(buffer, buffer_size, "%s%.1f%s", (value > 0.0f) ? "+" : "", (double)value, suffix);
}

static void weather_ui_format_temperature_rounded(float value, char* buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "%s%.0f°", (value >= 0.0f) ? "+" : "", (double)value);
}

static void weather_ui_format_wind(float value, const char* direction, char* buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "%.1f %s %s",
             (double)value,
             weather_ui_is_metric_units() ? "m/s" : "mph",
             direction ? direction : "-");
}

static const char* weather_ui_day_name(int wday)
{
    static const char* days_en[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* days_zh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    if (wday < 0 || wday > 6) return "---";
    return language_get_en() ? days_en[wday] : days_zh[wday];
}

static const char* weather_ui_month_name(int month)
{
    static const char* months_en[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* months_zh[] = {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"};
    if (month < 0 || month > 11) return "---";
    return language_get_en() ? months_en[month] : months_zh[month];
}

WeatherUI::WeatherUI() : 
    initialized_(false),
    time_update_timer_(nullptr),
    screen_(nullptr),
    prev_screen_(nullptr),
    screen_to_delete_(nullptr),
    deleting_(false),
    show_pending_(false) {
    city_label_ = nullptr;
    time_label_ = nullptr;
    date_label_ = nullptr;
    main_icon_ = nullptr;
    main_temp_label_ = nullptr;
    desc_label_ = nullptr;
    units_button_ = nullptr;
    units_label_ = nullptr;
    feels_like_label_ = nullptr;
    humidity_val_ = nullptr;
    pressure_val_ = nullptr;
    wind_val_ = nullptr;
    sunrise_val_ = nullptr;
    sunset_val_ = nullptr;
    sunrise_title_ = nullptr;
    sunset_title_ = nullptr;
    humidity_title_ = nullptr;
    pressure_title_ = nullptr;
    wind_title_ = nullptr;

    memset(forecast_labels_, 0, sizeof(forecast_labels_));
    memset(forecast_icons_, 0, sizeof(forecast_icons_));
    memset(forecast_temps_, 0, sizeof(forecast_temps_));
    memset(forecast_night_temps_, 0, sizeof(forecast_night_temps_));
    memset(forecast_descs_, 0, sizeof(forecast_descs_));
    memset(forecast_dates_, 0, sizeof(forecast_dates_));
    memset(forecast_precip_, 0, sizeof(forecast_precip_));
    memset(forecast_wind_, 0, sizeof(forecast_wind_));
}

WeatherUI::~WeatherUI() { clear(); }

WeatherUI* WeatherUI::instance_ = nullptr;

WeatherUI* WeatherUI::get_instance(void) {
    if (!instance_) {
        instance_ = new WeatherUI();
    }
    return instance_;
}

void WeatherUI::destroy_instance(void) {
    if (instance_) {
        delete instance_;
        instance_ = nullptr;
    }
}

bool WeatherUI::init() {
    if (initialized_) return true;
    screen_ = lv_obj_create(NULL);
    if (!screen_) return false;

    lv_obj_set_style_bg_color(screen_, COLOR_BG, 0);
    DISABLE_SCROLL(screen_);
    lv_obj_set_size(screen_, 800, 480);

    create_layout();
    create_left_side();
    create_right_side();

    time_update_timer_ = lv_timer_create(time_update_timer_cb, 15000, this);
    
    initialized_ = true;
    ESP_LOGI(TAG, "Weather UI initialized successfully with close button");
    return true;
}

void WeatherUI::show(void) {
    if (!lvgl_port_lock(-1)) {
        ESP_LOGW(TAG, "show(): failed to lock LVGL");
        return;
    }
    if (deleting_) {
        show_pending_ = true;
        ESP_LOGW(TAG, "show(): previous screen deletion pending, deferring show");
        lvgl_port_unlock();
        return;
    }
    if (!initialized_) {
        if (!init()) {
            ESP_LOGE(TAG, "Failed to init weather UI");
            lvgl_port_unlock();
            return;
        }
    }

    // FIX: do not overwrite prev_screen_ if show() is called again,
    // when the active screen is already weather screen_
    lv_obj_t *cur = lv_scr_act();
    if (cur && cur != screen_) {
        prev_screen_ = cur;
    }

    update_time_display();

    WeatherUI* ui = WeatherUI::get_instance();

    weather_data_t data;
    const weather_data_status_t status = weather_module_get_status();
    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED ||
        status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        ui->apply_status_state(status);
    } else if (weather_get_current_data(&data)) {
        ui->update_current_weather(&data);
        ESP_LOGI(TAG, "Updated weather data on show: %s, %.1f°C",
                 data.city, data.temperature);
    } else {
        ESP_LOGW(TAG, "No weather data available on show");
    }

    weather_forecast_day_t forecast[4];
    uint8_t count = 0;
    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED ||
        status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        ui->clear_forecast_display();
    } else if (weather_get_forecast_data(forecast, &count) && count > 0) {
        ui->update_forecast(forecast, count);
        ESP_LOGI(TAG, "Updated forecast on show: %d days", count);
    }

    lv_scr_load(screen_);
    ESP_LOGI(TAG, "Weather UI shown with updated data");
    lvgl_port_unlock();
}

void WeatherUI::delete_screen_async_cb(void* user_data)
{
    WeatherUI* self = static_cast<WeatherUI*>(user_data);
    if (!self) return;

    if (self->screen_to_delete_ && lv_obj_is_valid(self->screen_to_delete_)) {
        lv_obj_del(self->screen_to_delete_);
    }

    self->screen_to_delete_ = nullptr;
    self->deleting_ = false;

    if (self->show_pending_) {
        self->show_pending_ = false;
        self->show();
    }
}

void WeatherUI::hide(void) {
    if (!lvgl_port_lock(-1)) {
        ESP_LOGW(TAG, "hide(): failed to lock LVGL");
        return;
    }
    if (deleting_) {
        ESP_LOGW(TAG, "hide(): already deleting weather screen; ignoring");
        lvgl_port_unlock();
        return;
    }
    if (!initialized_ || !prev_screen_) {
        ESP_LOGW(TAG, "Cannot hide weather UI - no previous screen");
        lvgl_port_unlock();
        return;
    }
    if (!lv_obj_is_valid(prev_screen_)) {
        ESP_LOGW(TAG, "Cannot hide weather UI - previous screen is invalid");
        prev_screen_ = nullptr;
        lvgl_port_unlock();
        return;
    }

    lv_obj_t* screen_to_delete = screen_;

    lv_scr_load(prev_screen_);
    prev_screen_ = nullptr;

    if (time_update_timer_) {
        lv_timer_del(time_update_timer_);
        time_update_timer_ = nullptr;
    }

    // Detach LVGL object pointers immediately to prevent use-after-free in any queued work.
    screen_ = nullptr;
    left_panel_ = nullptr;
    right_panel_ = nullptr;
    city_label_ = nullptr;
    time_label_ = nullptr;
    date_label_ = nullptr;
    main_temp_label_ = nullptr;
    feels_like_label_ = nullptr;
    units_button_ = nullptr;
    units_label_ = nullptr;
    main_icon_ = nullptr;
    desc_label_ = nullptr;
    humidity_val_ = nullptr;
    pressure_val_ = nullptr;
    wind_val_ = nullptr;
    sunrise_val_ = nullptr;
    sunset_val_ = nullptr;
    sunrise_title_ = nullptr;
    sunset_title_ = nullptr;
    humidity_title_ = nullptr;
    pressure_title_ = nullptr;
    wind_title_ = nullptr;
    forecast_container_ = nullptr;
    memset(forecast_labels_, 0, sizeof(forecast_labels_));
    memset(forecast_icons_, 0, sizeof(forecast_icons_));
    memset(forecast_temps_, 0, sizeof(forecast_temps_));
    memset(forecast_night_temps_, 0, sizeof(forecast_night_temps_));
    memset(forecast_descs_, 0, sizeof(forecast_descs_));
    memset(forecast_dates_, 0, sizeof(forecast_dates_));
    memset(forecast_precip_, 0, sizeof(forecast_precip_));
    memset(forecast_wind_, 0, sizeof(forecast_wind_));

    initialized_ = false;

    if (screen_to_delete && lv_obj_is_valid(screen_to_delete)) {
        screen_to_delete_ = screen_to_delete;
        deleting_ = true;
        lv_res_t rc = lv_async_call(delete_screen_async_cb, this);
        if (rc != LV_RES_OK) {
            // Best-effort fallback: keep the screen for now; allow future show/hide.
            ESP_LOGE(TAG, "hide(): lv_async_call failed; cannot delete screen right now");
            screen_to_delete_ = nullptr;
            deleting_ = false;
        }
    }

    ESP_LOGI(TAG, "Weather UI hidden - returned to previous screen");
    lvgl_port_unlock();
}

void WeatherUI::set_weather_icon(lv_obj_t* obj, const char* name) {
    if (!obj || !name) return;
    
    const lv_img_dsc_t* src = &icon_01d;
    
    if (strstr(name, "01")) {
        src = (name[2] == 'd') ? &icon_01d : &icon_01n;
    }
    else if (strstr(name, "02")) {
        src = (name[2] == 'd') ? &icon_02d : &icon_02n;
    }
    else if (strstr(name, "03")) {
        src = &icon_03d;
    }
    else if (strstr(name, "04")) {
        src = &icon_04d;
    }
    else if (strstr(name, "09")) {
        src = &icon_09d;
    }
    else if (strstr(name, "10")) {
        src = (name[2] == 'd') ? &icon_10d : &icon_10n;
    }
    else if (strstr(name, "11")) {
        src = &icon_11d;
    }
    else if (strstr(name, "13")) {
        src = &icon_13d;
    }
    else if (strstr(name, "50")) {
        src = &icon_50d;
    }
    
    lv_img_set_src(obj, src);
}

void weather_ui_set_icon(lv_obj_t* icon_obj, const char* icon_name) {
    WeatherUI::set_weather_icon(icon_obj, icon_name);
}

void WeatherUI::create_layout() {
    left_panel_ = lv_obj_create(screen_);
    lv_obj_set_size(left_panel_, 300, 480);
    lv_obj_align(left_panel_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(left_panel_, 0, 0);
    lv_obj_set_style_border_width(left_panel_, 0, 0);
    lv_obj_set_style_pad_all(left_panel_, 20, 0);
    lv_obj_set_flex_flow(left_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left_panel_, 10, 0);
    DISABLE_SCROLL(left_panel_);

    right_panel_ = lv_obj_create(screen_);
    lv_obj_set_size(right_panel_, 500, 480);
    lv_obj_align(right_panel_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right_panel_, 0, 0);
    lv_obj_set_style_border_width(right_panel_, 0, 0);
    lv_obj_set_style_pad_all(right_panel_, 20, 0);
    DISABLE_SCROLL(right_panel_);
}

void WeatherUI::create_left_side() {
    lv_obj_t* top_spacer = lv_obj_create(left_panel_);
    lv_obj_set_height(top_spacer, lv_pct(50));
    lv_obj_set_width(top_spacer, 10);
    lv_obj_set_style_bg_opa(top_spacer, 0, 0);
    lv_obj_set_style_border_width(top_spacer, 0, 0);
    lv_obj_set_flex_grow(top_spacer, 1);
    DISABLE_SCROLL(top_spacer);

    city_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(city_label_, &ui_font_FontZh20, 0);
    lv_obj_set_style_text_color(city_label_, COLOR_TEXT_DIM, 0);
    lv_label_set_text(city_label_, weather_ui_text("Loading...", "正在加载..."));

    time_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_32, 0); 
    lv_obj_set_style_text_color(time_label_, COLOR_TEXT_MAIN, 0);
    lv_label_set_text(time_label_, "00:00");

    date_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(date_label_, &ui_font_FontZh16, 0);
    lv_obj_set_style_text_color(date_label_, COLOR_ACCENT, 0);
    lv_label_set_text(date_label_, "---");

    main_icon_ = lv_img_create(left_panel_);
    lv_img_set_zoom(main_icon_, 450); 
    lv_img_set_src(main_icon_, &icon_01d);

    main_temp_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(main_temp_label_, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(main_temp_label_, COLOR_TEXT_MAIN, 0);
    lv_label_set_text(main_temp_label_, weather_ui_is_metric_units() ? "--°C" : "--°F");

    desc_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(desc_label_, &ui_font_FontZh20, 0);
    lv_obj_set_style_text_color(desc_label_, COLOR_SECONDARY, 0);
    lv_label_set_text(desc_label_, "---");

    feels_like_label_ = lv_label_create(left_panel_);
    lv_obj_set_style_text_font(feels_like_label_, &ui_font_FontZh18, 0);
    lv_obj_set_style_text_color(feels_like_label_, COLOR_TEXT_DIM, 0);
    lv_label_set_text(feels_like_label_, "---");

    units_button_ = lv_btn_create(left_panel_);
    lv_obj_set_size(units_button_, 180, 42);
    lv_obj_set_style_bg_color(units_button_, COLOR_BUTTON, 0);
    lv_obj_set_style_bg_opa(units_button_, 150, 0);
    lv_obj_set_style_radius(units_button_, 18, 0);
    lv_obj_set_style_border_width(units_button_, 1, 0);
    lv_obj_set_style_border_color(units_button_, COLOR_CARD_BORDER, 0);
    lv_obj_set_style_shadow_width(units_button_, 0, 0);

    units_label_ = lv_label_create(units_button_);
    lv_obj_set_style_text_font(units_label_, &ui_font_Fonttt16, 0);
    lv_obj_set_style_text_color(units_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(units_label_);
    refresh_units_button();

    lv_obj_add_event_cb(units_button_, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
            return;
        }

        WeatherUI* ui = static_cast<WeatherUI*>(lv_event_get_user_data(e));
        weather_integration_set_metric_units(!weather_integration_get_metric_units());
        if (ui) {
            ui->refresh_units_button();
            ui->refresh_language();
        }
    }, LV_EVENT_CLICKED, this);
    
    lv_obj_t* spacer = lv_obj_create(left_panel_);
    lv_obj_set_height(spacer, lv_pct(50));
    lv_obj_set_width(spacer, 10);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);
    DISABLE_SCROLL(spacer);
    
    lv_obj_t* back_btn = lv_btn_create(left_panel_);
    lv_obj_set_size(back_btn, 180, 45);
    lv_obj_set_style_bg_color(back_btn, COLOR_BUTTON, 0);
    lv_obj_set_style_bg_opa(back_btn, 150, 0);
    lv_obj_set_style_radius(back_btn, 20, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_border_color(back_btn, COLOR_CARD_BORDER, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    
    lv_obj_t* arrow_label = lv_label_create(back_btn);
    lv_label_set_text(arrow_label, "<");
    lv_obj_set_style_text_font(arrow_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(arrow_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(arrow_label);
    
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_CLICKED) {
            WeatherUI* ui = WeatherUI::get_instance();
            if (ui) {
                ui->hide();
            }
        }
    }, LV_EVENT_CLICKED, this);
}

void WeatherUI::create_right_side() {
    lv_obj_t* cont = lv_obj_create(right_panel_);
    lv_obj_set_size(cont, 480, 460);
    lv_obj_center(cont);
    lv_obj_set_style_bg_opa(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 10, 0);
    DISABLE_SCROLL(cont);

    lv_obj_t* row1 = lv_obj_create(cont);
    lv_obj_set_size(row1, 460, 92);
    lv_obj_set_style_bg_opa(row1, 0, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row1, 20, 0);
    DISABLE_SCROLL(row1);

    auto create_sun_card_local = [&](const void* icon, const char* label, lv_obj_t** title_obj, lv_obj_t** val_obj) {
        lv_obj_t* card = lv_obj_create(row1);
        lv_obj_set_size(card, 220, 82);
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(card, 120, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, COLOR_CARD_BORDER, 0);
        
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(card, 6, 0);
        DISABLE_SCROLL(card);

        lv_obj_t* img = lv_img_create(card);
        lv_img_set_src(img, icon);
        lv_img_set_zoom(img, 200);

        lv_obj_t* inner = lv_obj_create(card);
        lv_obj_set_size(inner, 144, 62);
        lv_obj_set_style_bg_opa(inner, 0, 0);
        lv_obj_set_style_border_width(inner, 0, 0);
        lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(inner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(inner, 0, 0);
        lv_obj_set_style_pad_gap(inner, 0, 0);
        DISABLE_SCROLL(inner);

        *title_obj = lv_label_create(inner);
        lv_obj_set_width(*title_obj, 144);
        lv_label_set_text(*title_obj, label);
        lv_label_set_long_mode(*title_obj, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(*title_obj, &ui_font_FontZh14, 0);
        lv_obj_set_style_text_color(*title_obj, COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_align(*title_obj, LV_TEXT_ALIGN_CENTER, 0);

        *val_obj = lv_label_create(inner);
        lv_obj_set_width(*val_obj, 144);
        lv_obj_set_style_text_font(*val_obj, &ui_font_FontZh26, 0);
        lv_obj_set_style_text_color(*val_obj, COLOR_SUNSET, 0);
        lv_obj_set_style_text_align(*val_obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(*val_obj, "--:--");
    };

    create_sun_card_local(&sunrise_icon, weather_ui_text("Sunrise", "日出"), &sunrise_title_, &sunrise_val_);
    create_sun_card_local(&sunset_icon, weather_ui_text("Sunset", "日落"), &sunset_title_, &sunset_val_);

    lv_obj_t* row2 = lv_obj_create(cont);
    lv_obj_set_size(row2, 460, 100);
    lv_obj_set_style_bg_opa(row2, 0, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row2, 12, 0);
    DISABLE_SCROLL(row2);

    auto create_param_card = [&](const void* icon, const char* title, lv_obj_t** title_obj, lv_obj_t** val_obj) {
        lv_obj_t* card = lv_obj_create(row2);
        lv_obj_set_size(card, 144, 84);
        lv_obj_set_style_bg_opa(card, 0, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, COLOR_CARD_BORDER, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(card, 3, 0);
        DISABLE_SCROLL(card);

        lv_obj_t* img = lv_img_create(card);
        lv_img_set_src(img, icon);
        lv_img_set_zoom(img, 165);

        *title_obj = lv_label_create(card);
        lv_obj_set_width(*title_obj, 124);
        lv_label_set_text(*title_obj, title);
        lv_obj_set_style_text_font(*title_obj, &ui_font_FontZh16, 0);
        lv_obj_set_style_text_color(*title_obj, COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_align(*title_obj, LV_TEXT_ALIGN_CENTER, 0);

        *val_obj = lv_label_create(card);
        lv_obj_set_width(*val_obj, 124);
        lv_obj_set_style_text_font(*val_obj, &ui_font_FontZh20, 0);
        lv_obj_set_style_text_color(*val_obj, COLOR_ACCENT, 0);
        lv_obj_set_style_text_align(*val_obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(*val_obj, "--");
    };

    create_param_card(&humidity_icon, weather_ui_text("Humidity", "湿度"), &humidity_title_, &humidity_val_);
    create_param_card(&pressure_icon, weather_ui_text("Pressure", "气压"), &pressure_title_, &pressure_val_);
    create_param_card(&wind_icon, weather_ui_text("Wind", "风速"), &wind_title_, &wind_val_);

    lv_obj_t* row3 = lv_obj_create(cont);
    lv_obj_set_size(row3, 460, 248);
    lv_obj_set_style_bg_opa(row3, 0, 0);
    lv_obj_set_style_border_width(row3, 0, 0);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row3, 10, 0);
    DISABLE_SCROLL(row3);

    for (int i = 0; i < 3; i++) {
        lv_obj_t* f_card = lv_obj_create(row3); 
        lv_obj_set_size(f_card, 145, 232);
        lv_obj_set_style_bg_color(f_card, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(f_card, 150, 0);
        lv_obj_set_style_radius(f_card, 20, 0);
        
        lv_obj_set_style_border_width(f_card, 1, 0); 
        lv_obj_set_style_border_color(f_card, COLOR_CARD_BORDER, 0);
        
        lv_obj_set_flex_flow(f_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(f_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(f_card, 10, 0);
        lv_obj_set_style_pad_gap(f_card, 3, 0); 
        DISABLE_SCROLL(f_card);

        forecast_labels_[i] = lv_label_create(f_card);
        lv_obj_set_width(forecast_labels_[i], 121);
        lv_obj_set_style_text_font(forecast_labels_[i], &ui_font_FontZh18, 0);
        lv_obj_set_style_text_color(forecast_labels_[i], COLOR_TEXT_MAIN, 0);
        lv_obj_set_style_text_align(forecast_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(forecast_labels_[i], "---");
        lv_obj_set_style_pad_bottom(forecast_labels_[i], 0, 0);

        forecast_dates_[i] = lv_label_create(f_card);
        lv_obj_set_style_text_font(forecast_dates_[i], &ui_font_FontZh14, 0);
        lv_obj_set_style_text_color(forecast_dates_[i], COLOR_TEXT_DIM, 0);
        lv_label_set_text(forecast_dates_[i], "--.--");
        lv_obj_set_style_pad_bottom(forecast_dates_[i], 3, 0); 

        forecast_icons_[i] = lv_img_create(f_card);
        lv_img_set_zoom(forecast_icons_[i], 200); 
        lv_obj_set_style_pad_bottom(forecast_icons_[i], 0, 0);
        lv_obj_t* temp_container = lv_obj_create(f_card);
        lv_obj_set_size(temp_container, 121, 34);
        lv_obj_set_style_bg_opa(temp_container, 0, 0);
        lv_obj_set_style_border_width(temp_container, 0, 0);
        lv_obj_set_flex_flow(temp_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(temp_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(temp_container, 0, 0);
        lv_obj_set_style_pad_gap(temp_container, 2, 0);
        DISABLE_SCROLL(temp_container);
        
        forecast_temps_[i] = temp_container;
        lv_obj_set_style_pad_bottom(temp_container, 0, 0);
        lv_obj_set_style_translate_y(temp_container, -2, 0);

        lv_obj_t* day_temp_label = lv_label_create(temp_container);
        lv_obj_set_style_text_font(day_temp_label, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(day_temp_label, COLOR_ACCENT, 0);
        lv_label_set_text(day_temp_label, "--°");
        
        lv_obj_t* slash_label = lv_label_create(temp_container);
        lv_obj_set_style_text_font(slash_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(slash_label, COLOR_TEXT_DIM, 0);
        lv_label_set_text(slash_label, "/");
        
        lv_obj_t* night_temp_label = lv_label_create(temp_container);
        lv_obj_set_style_text_font(night_temp_label, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(night_temp_label, COLOR_NIGHT_TEMP, 0); 
        lv_label_set_text(night_temp_label, "--°");
        
        forecast_night_temps_[i] = night_temp_label;

        forecast_descs_[i] = lv_label_create(f_card);
        lv_obj_set_style_text_font(forecast_descs_[i], &ui_font_FontZh16, 0);
        lv_obj_set_style_text_color(forecast_descs_[i], COLOR_TEXT_DIM, 0);
        lv_label_set_long_mode(forecast_descs_[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(forecast_descs_[i], 121);
        lv_obj_set_height(forecast_descs_[i], 50);
        lv_obj_set_style_text_align(forecast_descs_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(forecast_descs_[i], "---");
        lv_obj_set_style_pad_bottom(forecast_descs_[i], 0, 0); 
    }
}

const char* WeatherUI::get_wind_direction_str(float degrees) {
    const bool en = language_get_en();
    if (degrees >= 337.5 || degrees < 22.5) return "N";
    if (degrees >= 22.5 && degrees < 67.5) return en ? "NE" : "NO";
    if (degrees >= 67.5 && degrees < 112.5) return en ? "E" : "O";
    if (degrees >= 112.5 && degrees < 157.5) return en ? "SE" : "SO";
    if (degrees >= 157.5 && degrees < 202.5) return "S";
    if (degrees >= 202.5 && degrees < 247.5) return "SW";
    if (degrees >= 247.5 && degrees < 292.5) return "W";
    if (degrees >= 292.5 && degrees < 337.5) return "NW";
    return "-";
}

void WeatherUI::format_precipitation_info(float pop, float rain, float snow, char* buffer, size_t buffer_size) {
    if (pop <= 0 && rain <= 0 && snow <= 0) {
        snprintf(buffer, buffer_size, "%s", weather_ui_text("No precipitation", "暂无降水"));
        return;
    }
    
    if (pop > 0 && rain <= 0 && snow <= 0) {
        snprintf(buffer, buffer_size, "%s: %.0f%%", weather_ui_text("Chance", "概率"), pop * 100);
        return;
    }
    
    if (snow > rain && snow > 0) {
        if (snow > 0) {
            snprintf(buffer, buffer_size, "%s: %.0f%% (%.1fmm)", weather_ui_text("Snow", "雪"), pop * 100, snow);
        } else {
            snprintf(buffer, buffer_size, "%s: %.0f%%", weather_ui_text("Snow", "雪"), pop * 100);
        }
    } else {
        if (rain > 0) {
            snprintf(buffer, buffer_size, "%s: %.0f%% (%.1fmm)", weather_ui_text("Rain", "雨"), pop * 100, rain);
        } else {
            snprintf(buffer, buffer_size, "%s: %.0f%%", weather_ui_text("Rain", "雨"), pop * 100);
        }
    }
}

void WeatherUI::clear_forecast_display(void) {
    for (int i = 0; i < 3; ++i) {
        if (forecast_labels_[i]) lv_label_set_text(forecast_labels_[i], "---");
        if (forecast_dates_[i]) lv_label_set_text(forecast_dates_[i], "--.--");
        if (forecast_icons_[i]) lv_img_set_src(forecast_icons_[i], NULL);
        if (forecast_temps_[i]) {
            lv_obj_t* day_temp_label = lv_obj_get_child(forecast_temps_[i], 0);
            lv_obj_t* slash_label = lv_obj_get_child(forecast_temps_[i], 1);
            if (day_temp_label) lv_label_set_text(day_temp_label, "--°");
            if (slash_label) lv_label_set_text(slash_label, "/");
        }
        if (forecast_night_temps_[i]) lv_label_set_text(forecast_night_temps_[i], "--°");
        if (forecast_descs_[i]) lv_label_set_text(forecast_descs_[i], "");
    }
}

void WeatherUI::apply_status_state(weather_data_status_t status) {
    const char* message = nullptr;
    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED) {
        message = "API key required";
    } else if (status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        message = "Invalid API key";
    }

    if (!message) {
        return;
    }

    if (city_label_) lv_label_set_text(city_label_, "");
    if (main_temp_label_) lv_label_set_text(main_temp_label_, weather_ui_is_metric_units() ? "--°C" : "--°F");
    if (feels_like_label_) lv_label_set_text(feels_like_label_, "");
    if (desc_label_) lv_label_set_text(desc_label_, message);
    if (main_icon_) set_weather_icon(main_icon_, "01d");
    if (humidity_val_) lv_label_set_text(humidity_val_, "--");
    if (pressure_val_) lv_label_set_text(pressure_val_, "--");
    if (wind_val_) lv_label_set_text(wind_val_, "--");
    if (sunrise_val_) lv_label_set_text(sunrise_val_, "--:--");
    if (sunset_val_) lv_label_set_text(sunset_val_, "--:--");
    clear_forecast_display();
}

void WeatherUI::update_current_weather(const weather_data_t* data) {
    if (!initialized_ || !data) return;
    const weather_data_status_t status = weather_module_get_status();
    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED ||
        status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        apply_status_state(status);
        return;
    }
    char buf[64];
    memcpy(&weather_data_, data, sizeof(weather_data_t));

    lv_label_set_text(city_label_, data->city[0] ? data->city : weather_ui_text("Loading...", "正在加载..."));

    weather_ui_format_temperature(data->temperature, buf, sizeof(buf), true);
    lv_label_set_text(main_temp_label_, buf);

    char feels_like_buf[24];
    weather_ui_format_temperature(data->feels_like, feels_like_buf, sizeof(feels_like_buf), false);
    snprintf(buf, sizeof(buf), "%s %s", weather_ui_text("Feels like", "体感温度"), feels_like_buf);
    lv_label_set_text(feels_like_label_, buf);

    lv_label_set_text(desc_label_, data->description);
    set_weather_icon(main_icon_, data->icon);

    snprintf(buf, sizeof(buf), "%.0f%%", (double)data->humidity);
    lv_label_set_text(humidity_val_, buf);

    const char* wind_dir = get_wind_direction_str(data->wind_direction);
    weather_ui_format_wind(data->wind_speed, wind_dir, buf, sizeof(buf));
    lv_label_set_text(wind_val_, buf);

    snprintf(buf, sizeof(buf), "%.0f hPa", (double)data->pressure);
    lv_label_set_text(pressure_val_, buf);

    if (data->sunrise > 0) {
        struct tm tm_info;
        time_t t = (time_t)data->sunrise;
        localtime_r(&t, &tm_info);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        lv_label_set_text(sunrise_val_, buf);
    }
    if (data->sunset > 0) {
        struct tm tm_info;
        time_t t = (time_t)data->sunset;
        localtime_r(&t, &tm_info);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        lv_label_set_text(sunset_val_, buf);
    }
}

void WeatherUI::update_forecast(const weather_forecast_day_t* forecast, uint8_t count) {
    if (!initialized_ || !forecast) return;
    char buf[64];

    for (int i = 0; i < count && i < 3; i++) {
        const weather_forecast_day_t* day = &forecast[i];
        
        if (forecast_labels_[i]) {
            lv_label_set_text(forecast_labels_[i], get_short_day_name(day->timestamp));
        }
        
        time_t t = (time_t)day->timestamp;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        snprintf(buf, sizeof(buf), "%02d.%02d", tm_info.tm_mday, tm_info.tm_mon + 1);
        if (forecast_dates_[i]) {
            lv_label_set_text(forecast_dates_[i], buf);
        }

        lv_obj_t* temp_container = forecast_temps_[i];
        if (temp_container) {
            lv_obj_t* day_temp_label = lv_obj_get_child(temp_container, 0);
            if (day_temp_label) {
                weather_ui_format_temperature_rounded(day->temp_day, buf, sizeof(buf));
                lv_label_set_text(day_temp_label, buf);
            }
            
            lv_obj_t* night_temp_label = forecast_night_temps_[i];
            if (night_temp_label) {
                weather_ui_format_temperature_rounded(day->temp_night, buf, sizeof(buf));
                lv_label_set_text(night_temp_label, buf);
            }
        }

        if (forecast_icons_[i]) {
            set_weather_icon(forecast_icons_[i], day->icon);
        }

        if (forecast_descs_[i]) {
            lv_label_set_text(forecast_descs_[i], day->description);
        }
        
        ESP_LOGI(TAG, "Forecast UI updated: %s, Day: %.1f%s, Night: %.1f%s",
                 get_day_name(day->timestamp),
                 day->temp_day,
                 weather_ui_is_metric_units() ? "C" : "F",
                 day->temp_night,
                 weather_ui_is_metric_units() ? "C" : "F");
    }
}

void WeatherUI::update_time_display() {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (now > 1000000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        if (time_label_) lv_label_set_text(time_label_, buf);

        snprintf(buf, sizeof(buf), "%s, %d %s",
                 weather_ui_day_name(timeinfo.tm_wday),
                 timeinfo.tm_mday,
                 weather_ui_month_name(timeinfo.tm_mon));
        if (date_label_) lv_label_set_text(date_label_, buf);
    }
}

void WeatherUI::time_update_timer_cb(lv_timer_t* timer) {
    WeatherUI* self = static_cast<WeatherUI*>(timer ? timer->user_data : nullptr);
    if (!self || !self->initialized_) return;
    self->update_time_display();
}

const char* WeatherUI::get_short_day_name(uint32_t timestamp) {
    static const char* days_en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* days_zh[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    time_t t = (time_t)timestamp;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    if (tm_info.tm_wday < 0 || tm_info.tm_wday > 6) return "---";
    return language_get_en() ? days_en[tm_info.tm_wday] : days_zh[tm_info.tm_wday];
}

const char* WeatherUI::get_day_name(uint32_t timestamp) {
    time_t t = (time_t)timestamp;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    return weather_ui_day_name(tm_info.tm_wday);
}

void WeatherUI::refresh_language(void) {
    if (!initialized_) return;
    const weather_data_status_t status = weather_module_get_status();

    if (sunrise_title_) lv_label_set_text(sunrise_title_, weather_ui_text("Sunrise", "日出"));
    if (sunset_title_) lv_label_set_text(sunset_title_, weather_ui_text("Sunset", "日落"));
    if (humidity_title_) lv_label_set_text(humidity_title_, weather_ui_text("Humidity", "湿度"));
    if (pressure_title_) lv_label_set_text(pressure_title_, weather_ui_text("Pressure", "气压"));
    if (wind_title_) lv_label_set_text(wind_title_, weather_ui_text("Wind", "风速"));
    refresh_units_button();

    weather_data_t data;
    weather_forecast_day_t forecast[4];
    uint8_t count = 0;

    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED ||
        status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        apply_status_state(status);
    } else if (weather_get_current_data(&data)) {
        update_current_weather(&data);
    } else if (city_label_) {
        lv_label_set_text(city_label_, weather_ui_text("Loading...", "正在加载..."));
        if (desc_label_) lv_label_set_text(desc_label_, weather_ui_text("Waiting for data...", "等待数据..."));
    }

    if (status == WEATHER_DATA_STATUS_API_KEY_REQUIRED ||
        status == WEATHER_DATA_STATUS_INVALID_API_KEY) {
        clear_forecast_display();
    } else if (weather_get_forecast_data(forecast, &count) && count > 0) {
        update_forecast(forecast, count);
    }

    update_time_display();
}

void WeatherUI::refresh_units_button(void) {
    if (!units_label_) return;

    lv_label_set_text(units_label_,
                      weather_ui_is_metric_units()
                          ? weather_ui_text("Units: C", "单位: 摄氏")
                          : weather_ui_text("Units: F", "单位: 华氏"));
}

void WeatherUI::set_time_synced(bool synced) {
    if (synced) {
        update_time_display();
    }
}

void WeatherUI::clear() {
    if (!lvgl_port_lock(-1)) {
        ESP_LOGW(TAG, "clear(): failed to lock LVGL");
        // Best-effort: prevent further use even if we couldn't free LVGL objects.
        initialized_ = false;
        time_update_timer_ = nullptr;
        screen_ = nullptr;
        prev_screen_ = nullptr;
        units_button_ = nullptr;
        units_label_ = nullptr;
        screen_to_delete_ = nullptr;
        deleting_ = false;
        show_pending_ = false;
        return;
    }

    (void)lv_async_call_cancel(delete_screen_async_cb, this);
    show_pending_ = false;
    deleting_ = false;

    if (time_update_timer_) {
        lv_timer_del(time_update_timer_);
        time_update_timer_ = nullptr;
    }

    if (screen_ && lv_obj_is_valid(screen_)) {
        lv_obj_del_async(screen_);
    }
    if (screen_to_delete_ && lv_obj_is_valid(screen_to_delete_)) {
        lv_obj_del_async(screen_to_delete_);
    }

    screen_ = nullptr;
    screen_to_delete_ = nullptr;
    left_panel_ = nullptr;
    right_panel_ = nullptr;
    city_label_ = nullptr;
    time_label_ = nullptr;
    date_label_ = nullptr;
    main_temp_label_ = nullptr;
    feels_like_label_ = nullptr;
    units_button_ = nullptr;
    units_label_ = nullptr;
    main_icon_ = nullptr;
    desc_label_ = nullptr;
    humidity_val_ = nullptr;
    pressure_val_ = nullptr;
    wind_val_ = nullptr;
    sunrise_val_ = nullptr;
    sunset_val_ = nullptr;
    sunrise_title_ = nullptr;
    sunset_title_ = nullptr;
    humidity_title_ = nullptr;
    pressure_title_ = nullptr;
    wind_title_ = nullptr;
    forecast_container_ = nullptr;
    memset(forecast_labels_, 0, sizeof(forecast_labels_));
    memset(forecast_icons_, 0, sizeof(forecast_icons_));
    memset(forecast_temps_, 0, sizeof(forecast_temps_));
    memset(forecast_night_temps_, 0, sizeof(forecast_night_temps_));
    memset(forecast_descs_, 0, sizeof(forecast_descs_));
    memset(forecast_dates_, 0, sizeof(forecast_dates_));
    memset(forecast_precip_, 0, sizeof(forecast_precip_));
    memset(forecast_wind_, 0, sizeof(forecast_wind_));
    prev_screen_ = nullptr;
    initialized_ = false;
    lvgl_port_unlock();
}
