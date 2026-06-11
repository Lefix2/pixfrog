idf_component_register(
    SRCS "src/sacn.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lwip
        esp_timer
        config_store
        dmx_manager
        led_protocols
        freertos
)
