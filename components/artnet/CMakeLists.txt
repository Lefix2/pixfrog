idf_component_register(
    SRCS "src/artnet.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lwip dmx_manager config_store fseq_player esp_common esp_hw_support
)
