# pixfrog top-level CMake — ESP-IDF v5.3+ on ESP32-P4
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pixfrog
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

# Strict C++17 across the project, no exceptions, no RTTI.
idf_build_set_property(CXX_STANDARD 17 APPEND)
idf_build_set_property(COMPILE_OPTIONS "-fno-exceptions;-fno-rtti" APPEND)
