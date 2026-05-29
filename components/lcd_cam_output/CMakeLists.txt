idf_component_register(
    SRCS "src/lcd_cam_output.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_common driver led_protocols dmx_manager freertos
)
