# Host-side unit tests for dmx_manager::logic (pure functions in dmx_logic.h).
# Build with:
#   cd components/dmx_manager/test && cmake -B build && cmake --build build && ./build/test_dmx_logic
cmake_minimum_required(VERSION 3.16)
project(test_dmx_logic CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(test_dmx_logic
    test_dmx_logic.cpp
    ../../led_protocols/src/led_protocols.cpp
    ../../led_protocols/src/encoder_nrz.cpp
    ../../led_protocols/src/encoder_spi.cpp
    ../../led_protocols/src/encoder_dmx.cpp
)
target_include_directories(test_dmx_logic PRIVATE
    ../include
    ../src
    ../../led_protocols/include
    ../../led_protocols/src
    ../../config_store/include
)
