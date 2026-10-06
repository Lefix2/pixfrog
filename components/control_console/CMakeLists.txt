idf_component_register(
    SRCS "src/control_console.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        console
        audio
        config_store
        dmx_manager
        fpp_sync
        fseq_player
        net
        led_output
        ui
        led_protocols
        sacn
        web_config
        esp_timer
        esp_app_format
        app_update
        esp_hw_support
        esp_system
        heap
)
