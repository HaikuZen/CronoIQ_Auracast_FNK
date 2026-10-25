idf_component_register(
    SRCS
        "main.cpp"
        "lvgl_display.cpp"
        "lvgl_mem_psram.cpp"
        "sd_card.cpp"
        "app_config.cpp"
        "config_store.cpp"
        "wifi_manager.cpp"
        "time_manager.cpp"
        "weather_service.cpp"
        "smart_lights_service.cpp"
        "env_sensor_service.cpp"
        "env_history.cpp"
        "ui/seven_segment.cpp"
        "ui/weather_icons.cpp"
        "ui/page_clock.cpp"
        "ui/page_weather.cpp"
        "ui/page_smart_lights.cpp"
        "ui/page_environment.cpp"
        "ui/page_settings.cpp"
        "ui/ui_manager.cpp"
    INCLUDE_DIRS
        "."
        "ui"
    REQUIRES
        driver
        esp_lcd
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_client
        mbedtls
        fatfs
        sdmmc
        lwip
        esp_timer
        esp_app_format
        bsec
)

# LVGL's custom-malloc hooks (lvgl_mem_psram.cpp) are only referenced from
# liblvgl, which links *after* libmain, so the linker would never pull that
# object in ("undefined reference to lv_malloc_core"). Force it, the same
# way IDF force-links app_main.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
