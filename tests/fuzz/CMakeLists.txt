# libFuzzer targets (clang, -DPIXFROG_FUZZ=ON). Under ctest each target only
# replays its seed corpus (a regression check); tools/fuzz.sh runs them for real.
set(FUZZ_CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus)

function(pixfrog_fuzz name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../harness
                                               ${FIRMWARE_INCLUDES}
                                               ${REPO}/components/fseq_player/src)
    target_compile_options(${name} PRIVATE -fno-exceptions -fno-rtti)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${name} PRIVATE idf_shims)
    add_test(NAME ${name} COMMAND ${name} -runs=0 ${FUZZ_CORPUS}/${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${PIXFROG_TEST_ENV}")
endfunction()

pixfrog_fuzz(fuzz_parsers fuzz_parsers.cpp)
pixfrog_fuzz(fuzz_receivers
    fuzz_receivers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../harness/fakes/fseq_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../harness/fakes/net_fake.cpp
    ${REPO}/components/artnet/src/artnet.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_fuzz(fuzz_console
    fuzz_console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../harness/fakes/fseq_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../harness/fakes/modules_fake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../harness/fakes/net_fake.cpp
    ${REPO}/components/control_console/src/control_console.cpp
    ${REPO}/components/sacn/src/sacn.cpp
    ${REPO}/components/dmx_manager/src/dmx_manager.cpp
    ${REPO}/components/config_store/src/config_store.cpp
    ${LED_PROTOCOLS_SRC}
)
pixfrog_fuzz(fuzz_web_api fuzz_web_api.cpp ${WEB_SOURCES})
target_link_libraries(fuzz_web_api PRIVATE cjson webui_blob)
target_compile_definitions(fuzz_web_api PRIVATE PIXFROG_HARNESS_REAL_WEB)
