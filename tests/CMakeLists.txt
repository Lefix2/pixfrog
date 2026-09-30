# Every host test suite in one CMake project, driven by ctest.
#
#   cmake -S tests -B build/tests && cmake --build build/tests --parallel
#   ctest --test-dir build/tests --output-on-failure
#
# Modes (CI runs all three):
#   -DPIXFROG_SANITIZE=ON  ASan + UBSan, any report fails the test
#   -DPIXFROG_COVERAGE=ON  gcov instrumentation, -O0 (see tools/coverage.sh)
#   -DPIXFROG_FUZZ=ON      clang only: ASan + UBSan + libFuzzer targets (tests/fuzz)
#
# The per-component test directories stay self-contained (each still builds on
# its own, as the host-tests skill documents); this file only aggregates them.
cmake_minimum_required(VERSION 3.16)
project(pixfrog_host_tests C CXX)

option(PIXFROG_SANITIZE "Build the suites with ASan + UBSan" OFF)
option(PIXFROG_COVERAGE "Build the suites with gcov instrumentation" OFF)
option(PIXFROG_FUZZ "Also build the libFuzzer targets (clang; implies sanitizers)" OFF)

if(PIXFROG_SANITIZE AND PIXFROG_COVERAGE)
    message(FATAL_ERROR "PIXFROG_SANITIZE and PIXFROG_COVERAGE are separate builds")
endif()
if(PIXFROG_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PIXFROG_FUZZ needs clang: CC=clang CXX=clang++ cmake ...")
    endif()
    if(PIXFROG_COVERAGE)
        message(FATAL_ERROR "PIXFROG_FUZZ and PIXFROG_COVERAGE are separate builds")
    endif()
    set(PIXFROG_SANITIZE ON)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()
if(PIXFROG_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()
if(PIXFROG_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

enable_testing()
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)

# component → executable its test directory builds
set(PIXFROG_SUITES
    "led_protocols:test_led_protocols"
    "dmx_manager:test_dmx_logic"
    "artnet:test_artnet_parser"
    "config_store:test_config_store"
    "sacn:test_sacn_parser"
    "fseq_player:test_fseq_parser"
    "fpp_sync:test_fpp_sync_parser"
)
foreach(entry IN LISTS PIXFROG_SUITES)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 component)
    list(GET parts 1 exe)
    add_subdirectory(${REPO}/components/${component}/test ${CMAKE_BINARY_DIR}/${component})
    add_test(NAME ${component} COMMAND ${exe})
endforeach()

set(PIXFROG_TEST_ENV
    "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
set_tests_properties(led_protocols dmx_manager artnet config_store sacn fseq_player fpp_sync
    PROPERTIES ENVIRONMENT "${PIXFROG_TEST_ENV}")

add_subdirectory(harness)
