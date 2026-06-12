# Host-side unit tests for the pure FSEQ parser (fseq_format.h).
# Build:
#   cd components/fseq_player/test
#   cmake -B build && cmake --build build && ./build/test_fseq_parser
cmake_minimum_required(VERSION 3.16)
project(test_fseq_parser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(test_fseq_parser test_fseq_parser.cpp)
target_include_directories(test_fseq_parser PRIVATE ../src)
