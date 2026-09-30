idf_component_register(
    SRCS "src/web_config.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_server
        config_store
        dmx_manager
        fpp_sync
        fseq_player
        led_output
        led_protocols
        sacn
        ui
        esp_system
        esp_timer
        esp_app_format
        app_update
        mbedtls
        json
        espcoredump
        esp_partition
        freertos
)

# The SPA ships gzipped (~226 KB → ~60 KB): httpd serves one request at a
# time, so every byte of a page load stalls the API polls behind it. Python's
# gzip with mtime=0 keeps the blob reproducible.
idf_build_get_property(python PYTHON)
set(web_ui_gz "${CMAKE_CURRENT_BINARY_DIR}/web_ui.html.gz")
add_custom_command(
    OUTPUT "${web_ui_gz}"
    COMMAND "${python}" -c
            "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web_ui.html" "${web_ui_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/web_ui.html"
    VERBATIM)
add_custom_target(web_ui_gz DEPENDS "${web_ui_gz}")
target_add_binary_data(${COMPONENT_LIB} "${web_ui_gz}" BINARY DEPENDS web_ui_gz)
