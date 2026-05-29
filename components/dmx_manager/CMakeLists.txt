idf_component_register(
    SRCS "src/dmx_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES led_protocols config_store esp_common freertos
)
