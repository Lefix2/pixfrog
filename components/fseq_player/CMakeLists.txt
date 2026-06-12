idf_component_register(
    SRCS "src/fseq_player.cpp" "third_party/zstddeclib.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store dmx_manager sdmmc fatfs esp_log esp_timer freertos
)

# Suppress warnings from the vendored single-file zstd decoder.
set_source_files_properties(third_party/zstddeclib.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-function -Wno-missing-field-initializers \
-Wno-maybe-uninitialized -Wno-implicit-fallthrough -std=c99"
)
