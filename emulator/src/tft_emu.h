// Emulator-facing accessors for the SDL TFT backend (tft_sdl.cpp).
// Exposes the internal 320x240 RGB565 framebuffer so main_emulator.cpp can
// upload it to an SDL texture / save it as a screenshot.

#pragma once

#include <cstdint>

const uint16_t* emu_fb_ptr();  // native-endian RGB565, width*height entries
int emu_fb_width();
int emu_fb_height();
