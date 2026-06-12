idf_component_register(
    SRCS "src/lcd_cam_output.cpp" "src/parlio_output.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_mm esp_common esp_timer driver esp_driver_parlio led_protocols dmx_manager config_store freertos
)
