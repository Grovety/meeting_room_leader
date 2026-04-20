// weather_ui.h
#pragma once

#include "weather_module.h"
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

class WeatherUI {
public:
    WeatherUI();
    ~WeatherUI();
    lv_obj_t* get_screen() const { return screen_; }
    bool is_initialized() const { return initialized_; }
    bool init(void);
    void update_current_weather(const weather_data_t* data);
    void update_forecast(const weather_forecast_day_t* forecast, uint8_t count);
    void refresh_language(void);
    void set_wifi_status(bool connected);
    void set_time_synced(bool synced);
    void clear(void);
    void show(void);  
    void hide(void); 
    static WeatherUI* get_instance(void);
    static void destroy_instance(void);
    static void set_weather_icon(lv_obj_t* icon_obj, const char* icon_name);
    
private:
    bool initialized_;
    lv_timer_t* time_update_timer_;
    
    lv_obj_t* screen_;
    lv_obj_t* left_panel_;
    lv_obj_t* right_panel_;
    
    lv_obj_t* city_label_;
    lv_obj_t* time_label_;
    lv_obj_t* date_label_;
    lv_obj_t* main_temp_label_;
    lv_obj_t* feels_like_label_;
    lv_obj_t* main_icon_;
    lv_obj_t* desc_label_;
    lv_obj_t* units_button_;
    lv_obj_t* units_label_;

    lv_obj_t* humidity_val_;
    lv_obj_t* pressure_val_;
    lv_obj_t* wind_val_;
    lv_obj_t* sunrise_val_;
    lv_obj_t* sunset_val_;
    lv_obj_t* sunrise_title_;
    lv_obj_t* sunset_title_;
    lv_obj_t* humidity_title_;
    lv_obj_t* pressure_title_;
    lv_obj_t* wind_title_;
    
    lv_obj_t* forecast_container_;
    lv_obj_t* forecast_labels_[3];
    lv_obj_t* forecast_icons_[3];
    lv_obj_t* forecast_temps_[3];
    lv_obj_t* forecast_night_temps_[3]; 
    lv_obj_t* forecast_descs_[3];
    lv_obj_t* forecast_dates_[3];
    lv_obj_t* forecast_precip_[3]; 
    lv_obj_t* forecast_wind_[3]; 

    weather_data_t weather_data_;
    
    void create_layout();
    void create_left_side();
    void create_right_side();
    void create_forecast_section();
    static WeatherUI* instance_; 
    lv_obj_t* prev_screen_;
    lv_obj_t* screen_to_delete_;
    bool deleting_;
    bool show_pending_;

    static void delete_screen_async_cb(void* user_data);
    const char* get_short_day_name(uint32_t timestamp);
    const char* get_day_name(uint32_t timestamp);
    void update_time_display();
    static void time_update_timer_cb(lv_timer_t* timer);
    void refresh_units_button(void);
    void clear_forecast_display(void);
    void apply_status_state(weather_data_status_t status);
    
    const char* get_wind_direction_str(float degrees);
    void format_precipitation_info(float pop, float rain, float snow, char* buffer, size_t buffer_size);
};

#ifdef __cplusplus
extern "C" {
#endif

void weather_ui_set_icon(lv_obj_t* icon_obj, const char* icon_name);

#ifdef __cplusplus
}
#endif
