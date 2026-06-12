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
        app_update
        led_protocols
        config_store
        dmx_manager
        artnet
        sacn
        led_output
        ui
        control_console
        web_config
        fseq_player
        fpp_sync
)
