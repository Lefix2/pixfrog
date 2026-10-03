idf_component_register(
    SRCS "src/audio.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_i2s esp_timer freertos
)
