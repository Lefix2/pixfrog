idf_component_register(
    SRCS "src/config_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash led_protocols
)
