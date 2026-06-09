cmake_minimum_required(VERSION 3.16)
project(splashgen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(splashgen splashgen.cpp)
# nanosvg pulls in <math.h>; link libm on platforms that need it.
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(splashgen ${MATH_LIB})
endif()
