# IDF-shim harness: the real firmware sources (not copies of their logic)
# compiled against tests/shims — fake clock, in-memory NVS, single-threaded
# FreeRTOS. One executable per component keeps their static state apart.
set(SHIMS ${CMAKE_CURRENT_SOURCE_DIR}/../shims)
add_library(idf_shims STATIC ${SHIMS}/src/shims.cpp ${SHIMS}/src/net.cpp ${SHIMS}/src/system.cpp
    ${SHIMS}/src/http.cpp ${SHIMS}/src/services.cpp)
target_include_directories(idf_shims PUBLIC ${SHIMS}/include)
target_compile_options(idf_shims PRIVATE -Wall -Wextra)

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
    ${REPO}/components/config_store/include
    ${REPO}/components/led_protocols/include
    ${REPO}/components/led_protocols/src
    ${REPO}/components/dmx_manager/include
    ${REPO}/components/dmx_manager/src
)

function(pixfrog_harness name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FIRMWARE_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
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

pixfrog_harness(harness_receivers
    test_receivers.cpp
    fakes/fseq_fake.cpp
    ${REPO}/components/artnet/src/artnet.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_harness(harness_console
    test_console.cpp
    fakes/fseq_fake.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/modules_fake.cpp
    ${REPO}/components/web_config/src/web_config.cpp
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
