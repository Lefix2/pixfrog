idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../boards"
    REQUIRES
        esp_eth
        esp_event
        esp_netif
        esp_timer
        esp_lcd
        nvs_flash
        led_protocols
        config_store
        dmx_manager
        artnet
        sacn
        lcd_cam_output
        ui
        control_console
        web_config
)
