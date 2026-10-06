# IDF-shim harness: the real firmware sources (not copies of their logic)
# compiled against tests/shims — fake clock, in-memory NVS, single-threaded
# FreeRTOS. One executable per component keeps their static state apart.
set(SHIMS ${CMAKE_CURRENT_SOURCE_DIR}/../shims)
add_library(idf_shims STATIC ${SHIMS}/src/shims.cpp ${SHIMS}/src/net.cpp ${SHIMS}/src/system.cpp
    ${SHIMS}/src/http.cpp ${SHIMS}/src/services.cpp ${SHIMS}/src/vfs.cpp
    ${SHIMS}/src/output.cpp ${SHIMS}/src/lcd.cpp
    ${SHIMS}/src/ui_hw.cpp ${SHIMS}/src/boot.cpp ${SHIMS}/src/i2s.cpp)
target_include_directories(idf_shims PUBLIC ${SHIMS}/include)
# Same C++ dialect as the firmware: device models defined in a test (no RTTI)
# are called through the shims, which UBSan would otherwise vptr-check.
target_compile_options(idf_shims PRIVATE -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

set(LED_PROTOCOLS_SRC
    ${REPO}/components/led_protocols/src/led_protocols.cpp
    ${REPO}/components/led_protocols/src/encoder_frame.cpp
    ${REPO}/components/led_protocols/src/encoder_nrz.cpp
    ${REPO}/components/led_protocols/src/encoder_spi.cpp
)
set(FIRMWARE_INCLUDES
    ${REPO}/components/artnet/include
    ${REPO}/components/sacn/include
    ${REPO}/components/fseq_player/include
    ${REPO}/components/control_console/include
    ${REPO}/components/fpp_sync/include
    ${REPO}/components/led_output/include
    ${REPO}/components/ui/include
    ${REPO}/components/web_config/include
    ${REPO}/components/net/include
    ${REPO}/components/audio/include
    ${REPO}/components/web_config/src
    ${REPO}/components/config_store/include
    ${REPO}/components/led_protocols/include
    ${REPO}/components/led_protocols/src
    ${REPO}/components/dmx_manager/include
    ${REPO}/components/dmx_manager/src
)

function(pixfrog_harness name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FIRMWARE_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
        # /sdcard/... file calls land in the host directory the test picked.
        -include ${SHIMS}/include/vfs_redirect.h)
    target_link_libraries(${name} PRIVATE idf_shims)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${PIXFROG_TEST_ENV}")
endfunction()

pixfrog_harness(harness_config_store
    test_config_store_nvs.cpp
    ${REPO}/components/config_store/src/config_store.cpp
)
pixfrog_harness(harness_dmx_manager
    test_dmx_manager.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_harness(harness_dmx_init
    test_dmx_init.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
# Failed inits leak their partial allocations by design (boot aborts on the
# device): no leak check for this one.
set_tests_properties(harness_dmx_init PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")

# The real FSEQ player (SD hot-plug, uncompressed / zstd / sparse files,
# seek, desk requests) and the FPP MultiSync receiver driving it.
pixfrog_harness(harness_fseq
    test_fseq.cpp
    ${REPO}/components/fseq_player/src/fseq_player.cpp
    ${REPO}/components/fseq_player/third_party/zstddeclib.c
    ${REPO}/components/fpp_sync/src/fpp_sync.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
target_include_directories(harness_fseq PRIVATE ${REPO}/components/fseq_player/src
                                                ${REPO}/components/fseq_player/third_party)
set_source_files_properties(${REPO}/components/fseq_player/third_party/zstddeclib.c PROPERTIES
    COMPILE_OPTIONS "-w")

# The PARLIO LED output backend against a recording PARLIO driver: what it
# would put on the bus is decoded back to pixel bytes.
# Both backends are compiled, as on the device; the Kconfig choice keeps one.
set(LED_OUTPUT_SRC ${REPO}/components/led_output/src/parlio_output.cpp
    ${REPO}/components/led_output/src/lcd_cam_output.cpp)
pixfrog_harness(harness_output
    test_output.cpp
    ${LED_OUTPUT_SRC}
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
# The legacy LCD_CAM backend against a recording RGB panel.
pixfrog_harness(harness_output_lcdcam
    test_output_lcdcam.cpp
    ${LED_OUTPUT_SRC}
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
target_compile_definitions(harness_output_lcdcam PRIVATE CONFIG_PIXFROG_LED_OUTPUT_PARLIO=0)
foreach(t harness_output harness_output_lcdcam)
    target_include_directories(${t} PRIVATE ${REPO}/components/led_output/src)
    # Each case re-inits the backend, which allocates its buffers once per boot
    # on the device (never freed by design): no leak check here.
    set_tests_properties(${t} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endforeach()

pixfrog_harness(harness_receivers
    test_receivers.cpp
    fakes/fseq_fake.cpp
    fakes/net_fake.cpp
    ${REPO}/components/artnet/src/artnet.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_harness(harness_console
    test_console.cpp
    fakes/fseq_fake.cpp
    fakes/net_fake.cpp
    fakes/modules_fake.cpp
    ${REPO}/components/control_console/src/control_console.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)

# web_config: the real handlers + cJSON (vendored, the library IDF ships) +
# the gzipped SPA linked in as the same _binary_web_ui_html_gz_* symbols the
# firmware's EMBED gets.
add_library(cjson STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cJSON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(WEB_GZ_DIR ${CMAKE_CURRENT_BINARY_DIR}/webui)
file(MAKE_DIRECTORY ${WEB_GZ_DIR})
add_custom_command(
    OUTPUT ${WEB_GZ_DIR}/web_ui_blob.o
    COMMAND ${Python3_EXECUTABLE} -c
            "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            ${REPO}/components/web_config/src/web_ui.html ${WEB_GZ_DIR}/web_ui.html.gz
    COMMAND ld -r -b binary -z noexecstack -o web_ui_blob.o web_ui.html.gz
    WORKING_DIRECTORY ${WEB_GZ_DIR}
    DEPENDS ${REPO}/components/web_config/src/web_ui.html
    VERBATIM)

# A library, so targets in other directories (tests/fuzz) can link the blob.
add_library(webui_blob STATIC ${WEB_GZ_DIR}/web_ui_blob.o)
set_target_properties(webui_blob PROPERTIES LINKER_LANGUAGE C)

set(WEB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fseq_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/net_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/modules_fake.cpp
    ${REPO}/components/web_config/src/web_config.cpp
    ${REPO}/components/web_config/src/web_util.cpp
    ${REPO}/components/web_config/src/api_show.cpp
    ${REPO}/components/web_config/src/api_config.cpp
    ${REPO}/components/web_config/src/api_status.cpp
    ${REPO}/components/web_config/src/web_push.cpp
    ${REPO}/components/web_config/src/api_system.cpp
    ${REPO}/components/web_config/src/api_fseq.cpp
    ${REPO}/components/web_config/src/web_mdns.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_harness(harness_web test_web.cpp ${WEB_SOURCES})
target_link_libraries(harness_web PRIVATE cjson webui_blob)
target_compile_definitions(harness_web PRIVATE PIXFROG_HARNESS_REAL_WEB)

# The real API on a TCP port for the SPA browser tests (tests/web).
add_executable(pixfrog_api_host ${CMAKE_CURRENT_SOURCE_DIR}/../web/api_host.cpp ${WEB_SOURCES})
target_include_directories(pixfrog_api_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FIRMWARE_INCLUDES})
target_compile_options(pixfrog_api_host PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_compile_definitions(pixfrog_api_host PRIVATE PIXFROG_HARNESS_REAL_WEB)
target_link_libraries(pixfrog_api_host PRIVATE idf_shims cjson webui_blob)

# libFuzzer targets reuse the source lists above (a child scope sees them).
if(PIXFROG_FUZZ)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../fuzz ${CMAKE_BINARY_DIR}/fuzz)
endif()

# The speaker: the real audio driver on the I2C + I2S shims, an ES8311 model.
pixfrog_harness(harness_audio test_audio.cpp ${REPO}/components/audio/src/audio.cpp)
target_include_directories(harness_audio PRIVATE ${REPO}/components/audio/src)

# The UI hardware layer (display, encoder, backlight drivers) and ui.cpp's task
# loop, once per display Kconfig; menu.cpp is faked (the emulator covers it).
set(UI_SRC ${REPO}/components/ui/src)
set(UI_COMMON test_ui.cpp fakes/menu_fake.cpp fakes/audio_fake.cpp ${UI_SRC}/ui.cpp ${UI_SRC}/encoder_seesaw.cpp
    ${UI_SRC}/splash.cpp ${REPO}/components/config_store/src/config_store.cpp)
set(UI_TFT ${UI_SRC}/canvas_tft.cpp ${UI_SRC}/backlight.cpp ${UI_SRC}/tft_backlight_pwm.cpp
    ${UI_SRC}/font_data.cpp ${UI_SRC}/splash_anim.cpp)
pixfrog_harness(harness_ui_oled ${UI_COMMON} ${UI_SRC}/oled_ssd1306.cpp ${UI_SRC}/canvas_oled.cpp
    ${UI_SRC}/font_oled.cpp ${UI_SRC}/splash_oled.cpp)
pixfrog_harness(harness_ui_nv3007 ${UI_COMMON} ${UI_TFT} ${UI_SRC}/tft_nv3007.cpp)
target_compile_definitions(harness_ui_nv3007 PRIVATE CONFIG_PIXFROG_DISPLAY_TFT=1
    CONFIG_PIXFROG_DISPLAY_NV3007=1 CONFIG_CACHE_L1_CACHE_LINE_SIZE=64)
pixfrog_harness(harness_ui_nv3007_rot180 ${UI_COMMON} ${UI_TFT} ${UI_SRC}/tft_nv3007.cpp)
target_compile_definitions(harness_ui_nv3007_rot180 PRIVATE CONFIG_PIXFROG_DISPLAY_TFT=1
    CONFIG_PIXFROG_DISPLAY_NV3007=1 CONFIG_PIXFROG_NV3007_ROT180=1
    CONFIG_CACHE_L1_CACHE_LINE_SIZE=64)
pixfrog_harness(harness_ui_st7789 ${UI_COMMON} ${UI_TFT} ${UI_SRC}/tft_st7789.cpp)
target_compile_definitions(harness_ui_st7789 PRIVATE CONFIG_PIXFROG_DISPLAY_TFT=1)
foreach(t harness_ui_oled harness_ui_nv3007 harness_ui_nv3007_rot180 harness_ui_st7789)
    target_include_directories(${t} PRIVATE ${UI_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
    # Buses, devices and panels are created once per boot on the device; the
    # cases re-create them freely.
    set_tests_properties(${t} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endforeach()

# main.cpp: app_main() boots every real module (OLED UI build, menu faked) and
# the render task is driven frame by frame — Art-Net in, PARLIO wire out.
pixfrog_harness(harness_boot
    test_boot.cpp
    fakes/menu_fake.cpp
    ${REPO}/main/main.cpp
    ${REPO}/components/net/src/net.cpp
    ${REPO}/components/artnet/src/artnet.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/fpp_sync/src/fpp_sync.cpp
    ${REPO}/components/fseq_player/src/fseq_player.cpp
    ${REPO}/components/fseq_player/third_party/zstddeclib.c
    ${REPO}/components/control_console/src/control_console.cpp
    ${REPO}/components/web_config/src/web_config.cpp
    ${REPO}/components/web_config/src/web_util.cpp
    ${REPO}/components/web_config/src/api_show.cpp
    ${REPO}/components/web_config/src/api_config.cpp
    ${REPO}/components/web_config/src/api_status.cpp
    ${REPO}/components/web_config/src/web_push.cpp
    ${REPO}/components/web_config/src/api_system.cpp
    ${REPO}/components/web_config/src/api_fseq.cpp
    ${REPO}/components/web_config/src/web_mdns.cpp
    ${REPO}/components/audio/src/audio.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_OUTPUT_SRC}
    ${LED_PROTOCOLS_SRC}
    ${UI_SRC}/ui.cpp ${UI_SRC}/encoder_seesaw.cpp ${UI_SRC}/splash.cpp ${UI_SRC}/oled_ssd1306.cpp
    ${UI_SRC}/canvas_oled.cpp ${UI_SRC}/font_oled.cpp ${UI_SRC}/splash_oled.cpp
)
target_include_directories(harness_boot PRIVATE ${REPO}/boards ${REPO}/components/audio/src ${REPO}/components/led_output/src
    ${REPO}/components/fseq_player/src ${REPO}/components/fseq_player/third_party ${UI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_link_libraries(harness_boot PRIVATE cjson webui_blob)
# app_main() runs once per case; the device runs it once per boot.
set_tests_properties(harness_boot PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
