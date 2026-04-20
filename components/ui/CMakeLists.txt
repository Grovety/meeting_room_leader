set(SRCS 
    device_name_store.c
    ui_Screen1.c
    ui_Screen8.c
    ui_ScreenInfo.c
    ui.c
    ui_time.c 
    ui_helpers.c
    fonts/ui_font_FontZh12.c
    fonts/ui_font_FontZh14.c
    fonts/ui_font_FontZh16.c
    fonts/ui_font_Fonttt16.c
    fonts/ui_font_FontZh18.c
    fonts/ui_font_FontZh20.c
    fonts/ui_font_FontZh26.c
    fonts/ui_font_FontZh30.c
    fonts/ui_font_FontZhh50.c
    fonts/ui_font_Roboto40.c
    fonts/ui_font_Roboto50.c
    fonts/ui_font_Roboto55.c
    fonts/ui_font_Roboto60.c
    images/ui_img_630012529.c
    images/ui_img_1474184779.c
    images/ui_img_infoicon_png.c
    images/ui_img_wifi32_png.c
    images/ui_img_settingsbuttonan_png.c
    images/ui_img_653468872.c
    images/ui_img_wifiicon13_png.c
    images/ui_img_smartphone_png.c
    images/ui_img_nameicon_png.c
    images/ui_img_moonicon_png.c
)


set(INCLUDE_DIRS "include")


idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES lvgl__lvgl espressif__esp32_display_panel nvs_flash language_manager esp_timer
    PRIV_REQUIRES weather_module main
)
