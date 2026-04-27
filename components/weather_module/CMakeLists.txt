idf_component_register(
    SRCS
    "weather_module.cpp"
    "weather_service.cpp"
    "weather_adapter.cpp"
    "weather_ui.cpp"
    "weather_integration.cpp"
    "weather_widget.cpp"
    "images/01d.c"
    "images/01n.c"
    "images/02d.c"
    "images/02n.c"
    "images/03d.c"
    "images/03n.c"
    "images/04d.c"
    "images/04n.c"
    "images/09d.c"
    "images/09n.c"
    "images/10d.c"
    "images/10n.c"
    "images/11d.c"
    "images/11n.c"
    "images/13d.c"
    "images/13n.c"
    "images/50d.c"
    "images/50n.c"
    "images/humidity.c"
    "images/pressure.c"
    "images/wind.c"
    "images/sunrise.c"
    "images/sunset.c"
    
    
    INCLUDE_DIRS
    "."
    "images"
    ""
    
    REQUIRES
    lvgl
    esp_http_client
    json_parser
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    lwip
    esp_timer
    language_manager
    network_request_guard
    PRIV_REQUIRES
    json
    main
    mbedtls
)

set_source_files_properties(
    images/*.c
    PROPERTIES COMPILE_FLAGS "-Wno-format"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-missing-field-initializers")
