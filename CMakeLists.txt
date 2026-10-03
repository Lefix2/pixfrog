# pixfrog top-level CMake — ESP-IDF v5.5+ on ESP32-P4
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
)

# Firmware version = git describe: exact tag (v0.1.4) on a tagged commit,
# tag + distance + hash (v0.1.4-36-g1d010ad[-dirty]) past it. PROJECT_VER set
# before project() takes precedence over the VERSION argument in IDF, and
# lands in esp_app_get_description()->version. Falls back to the static
# VERSION below when git is unavailable (e.g. source tarball builds).
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PIXFROG_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PIXFROG_GIT_RESULT
    ERROR_QUIET
)
if(PIXFROG_GIT_RESULT EQUAL 0 AND NOT PIXFROG_GIT_VERSION STREQUAL "")
    set(PROJECT_VER ${PIXFROG_GIT_VERSION})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# sACN joins one multicast group per universe through the socket API, whose
# membership table is LWIP_SOCKET_MAX_MEMBERSHIPS entries — by default
# CONFIG_LWIP_MAX_SOCKETS (10), so universes past the 10th never received
# multicast (measured on the bench: 16 asked, 6 "IGMP join failed"). Sized for
# the universe pool (dmx::kNumUniverses = 64) + the control universe + spare.
# The IGMP groups themselves come from the heap (MEMP_MEM_MALLOC), no cap there.
idf_build_set_property(COMPILE_DEFINITIONS "LWIP_SOCKET_MAX_MEMBERSHIPS=72" APPEND)

project(pixfrog
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

# Strict C++17 across the project, no exceptions, no RTTI.
# These flags are C++-only; applying them to C sources (e.g. mbedtls) fails
# under -Werror with "option is valid for C++ ... but not for C".
idf_build_set_property(CXX_STANDARD 17)
idf_build_set_property(CXX_COMPILE_OPTIONS "-fno-exceptions;-fno-rtti" APPEND)
