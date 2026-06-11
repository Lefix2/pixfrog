# Host-side unit tests for config_store struct layout and NVS migration logic.
# Build: cd components/config_store/test && cmake -B build && cmake --build build && ./build/test_config_store
cmake_minimum_required(VERSION 3.16)
project(test_config_store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(test_config_store test_config_store.cpp)
target_include_directories(test_config_store PRIVATE
    ../include
    ../../led_protocols/include
)
