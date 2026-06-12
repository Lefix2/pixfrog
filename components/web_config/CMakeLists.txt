idf_component_register(
    SRCS "src/web_config.cpp"
    INCLUDE_DIRS "include"
    EMBED_FILES "src/web_ui.html"
    PRIV_REQUIRES
        esp_http_server
        config_store
        dmx_manager
        fpp_sync
        fseq_player
        led_protocols
        sacn
        ui
        esp_system
        esp_timer
        esp_app_format
        app_update
        mbedtls
        json
        espcoredump
        esp_partition
        freertos
)
