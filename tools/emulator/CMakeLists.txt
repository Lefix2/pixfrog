# Standalone host build of the pixfrog UI emulator (no ESP-IDF).
#
# Reuses the device's real UI sources (menu/canvas_tft/splash/font) compiled
# with CONFIG_PIXFROG_DISPLAY_TFT, and replaces the hardware backends with SDL2
# + in-RAM stubs. See README.md.
#
# Build:
#   sudo apt install libsdl2-dev
#   cd tools/emulator && cmake -B build && cmake --build build
#   ./build/pixfrog_emu                 # interactive window
#   ./build/pixfrog_emu --headless      # agent/CI, piloted via stdin
cmake_minimum_required(VERSION 3.16)
project(pixfrog_emu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

find_package(SDL2 REQUIRED)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(pixfrog_emu
    # device UI sources, reused verbatim
    ${REPO}/components/ui/src/menu.cpp
    ${REPO}/components/ui/src/canvas_tft.cpp
    ${REPO}/components/ui/src/splash.cpp
    ${REPO}/components/ui/src/splash_anim.cpp
    ${REPO}/components/ui/src/font_data.cpp
    # (led_protocols: only header constexprs/enums are used — no .cpp needed)
    # emulator backends + stubs
    src/tft_sdl.cpp
    src/encoder_host.cpp
    src/config_host.cpp
    src/dmx_host.cpp
    src/fseq_host.cpp
    src/led_output_host.cpp
    src/web_host.cpp
    src/sacn_host.cpp
    src/main_emulator.cpp
)

target_compile_definitions(pixfrog_emu PRIVATE CONFIG_PIXFROG_DISPLAY_TFT PIXFROG_EMULATOR)

target_include_directories(pixfrog_emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shims          # driver/i2c_master.h shim (must precede IDF)
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REPO}/components/ui/src
    ${REPO}/components/ui/include
    ${REPO}/components/config_store/include
    ${REPO}/components/dmx_manager/include
    ${REPO}/components/fseq_player/include
    ${REPO}/components/led_output/include
    ${REPO}/components/led_protocols/include
    ${REPO}/components/web_config/include
    ${REPO}/components/sacn/include
)

target_link_libraries(pixfrog_emu PRIVATE SDL2::SDL2)
