idf_component_register(
    SRCS "src/control_console.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        console
        config_store
        dmx_manager
        lcd_cam_output
        ui
        led_protocols
        esp_timer
        esp_app_format
        esp_hw_support
        esp_system
        heap
)
