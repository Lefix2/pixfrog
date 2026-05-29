# pixfrog top-level CMake — ESP-IDF v5.5+ on ESP32-P4
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
# These flags are C++-only; applying them to C sources (e.g. mbedtls) fails
# under -Werror with "option is valid for C++ ... but not for C".
idf_build_set_property(CXX_STANDARD 17)
idf_build_set_property(CXX_COMPILE_OPTIONS "-fno-exceptions;-fno-rtti" APPEND)
