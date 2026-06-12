idf_component_register(
    SRCS "src/fpp_sync.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lwip fseq_player esp_common freertos
)
