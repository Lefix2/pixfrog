# IDF-shim harness: the real firmware sources (not copies of their logic)
# compiled against tests/shims — fake clock, in-memory NVS, single-threaded
# FreeRTOS. One executable per component keeps their static state apart.
set(SHIMS ${CMAKE_CURRENT_SOURCE_DIR}/../shims)
add_library(idf_shims STATIC ${SHIMS}/src/shims.cpp ${SHIMS}/src/net.cpp ${SHIMS}/src/system.cpp)
target_include_directories(idf_shims PUBLIC ${SHIMS}/include)
target_compile_options(idf_shims PRIVATE -Wall -Wextra)

set(LED_PROTOCOLS_SRC
    ${REPO}/components/led_protocols/src/led_protocols.cpp
    ${REPO}/components/led_protocols/src/encoder_frame.cpp
    ${REPO}/components/led_protocols/src/encoder_nrz.cpp
    ${REPO}/components/led_protocols/src/encoder_spi.cpp
    ${REPO}/components/led_protocols/src/encoder_dmx.cpp
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
