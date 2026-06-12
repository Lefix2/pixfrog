# Host-side unit tests for fpp::parser (header-only pure code).
cmake_minimum_required(VERSION 3.16)
project(test_fpp_sync_parser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(test_fpp_sync_parser test_fpp_sync_parser.cpp)
target_include_directories(test_fpp_sync_parser PRIVATE ../include)
