idf_component_register(
    SRCS
        "src/led_protocols.cpp"
        "src/encoder_nrz.cpp"
        "src/encoder_spi.cpp"
        "src/encoder_dmx.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
)
