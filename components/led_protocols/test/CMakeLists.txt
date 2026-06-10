# Host-side unit tests for led_protocols.
# Build with: cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols
cmake_minimum_required(VERSION 3.16)
project(test_led_protocols CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(test_led_protocols
    test_led_protocols.cpp
    ../src/led_protocols.cpp
    ../src/encoder_frame.cpp
    ../src/encoder_nrz.cpp
    ../src/encoder_spi.cpp
    ../src/encoder_dmx.cpp
)
target_include_directories(test_led_protocols PRIVATE ../include ../src)
