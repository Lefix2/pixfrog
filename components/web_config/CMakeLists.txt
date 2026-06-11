idf_component_register(
    SRCS "src/web_config.cpp"
    INCLUDE_DIRS "include"
    EMBED_FILES "src/web_ui.html"
    PRIV_REQUIRES
        esp_http_server
        config_store
        dmx_manager
        led_protocols
        ui
        esp_system
        json
        freertos
)
