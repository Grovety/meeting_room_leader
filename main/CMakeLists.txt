idf_component_register(
    SRCS
    "lvgl_v8_port.cpp"
    "main.cpp"
    "location_service.cpp"
    "hm_ctrl/crc.c"
    "hm_ctrl/crc_table.c"
    "hm_ctrl/hm_comm_protocol.c"
    "hm_ctrl/hm_regs.c"
    "uart_json.c"
    "uart_manager.c"
    "wifi_manager.c"
    "aux_udp_link.cpp"
    "time_sync.c"
    "sleep_manager.cpp"
    "i2c_master.c"
    "rtc_ds3231.c"
    "info_links_store.c"
    "config_portal.c"
    INCLUDE_DIRS
    .
    "hm_ctrl"
    "include"
    PRIV_REQUIRES
    json
    espressif__esp32_io_expander
    mbedtls
    fatfs
    REQUIRES
    driver
    esp_timer
    lvgl__lvgl
    ui
    esp_event
    esp_netif
    nvs_flash
    esp_wifi
    esp_http_server
    esp_http_client
    lwip
    weather_module
    network_request_guard
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(COMPONENT ${build_components})
    if(COMPONENT MATCHES "lvgl" OR COMPONENT MATCHES "lvgl__lvgl")
        set(TARGET_COMPONENT ${COMPONENT})
        break()
    endif()
endforeach()

if(TARGET_COMPONENT STREQUAL "")
    message(FATAL_ERROR "Component 'lvgl' not found.")
else()
    idf_component_get_property(LVGL_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
endif()
target_compile_options(${LVGL_LIB} PRIVATE "-Wno-format")
set(TARGET_COMPONENT "")
