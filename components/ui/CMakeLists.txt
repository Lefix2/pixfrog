idf_component_register(
    SRCS
        "src/ui.cpp"
        "src/menu.cpp"
        "src/oled_ssd1306.cpp"
        "src/encoder_seesaw.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common config_store dmx_manager freertos
)
