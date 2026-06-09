// SDL/host implementation of the TFT low-level backend.
//
// Drop-in replacement for components/ui/src/tft_st7789.cpp: provides the same
// tft_init / tft_draw_bitmap / tft_width / tft_height symbols that canvas_tft.cpp
// calls. Instead of pushing pixels over SPI to an ST7789, it maintains an
// in-memory RGB565 framebuffer that main_emulator.cpp uploads to an SDL texture.
//
// canvas_tft.cpp byte-swaps every colour to big-endian (ST7789 wire order) via
// __builtin_bswap16 before calling tft_draw_bitmap. We swap back here so the
// framebuffer holds native-endian RGB565, ready for RGB565->RGB888 conversion.

#include "ui_internal.h"
#include "tft_emu.h"

#include <cstdint>

namespace pixfrog::ui::detail {

namespace {

constexpr int kW = 320;  // landscape width
constexpr int kH = 240;  // landscape height

uint16_t s_fb[kW * kH];  // native-endian RGB565

}  // namespace

bool tft_init(const TftConfig& /*cfg*/) {
    for (int i = 0; i < kW * kH; ++i)
        s_fb[i] = 0;
    return true;
}

// data is big-endian RGB565 (one row per scan-line call from canvas_tft.cpp).
// Rectangle is [x1,x2) x [y1,y2). Clipped to the framebuffer bounds.
void tft_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t* data) {
    if (!data) return;
    const int rect_w = x2 - x1;
    if (rect_w <= 0 || y2 <= y1) return;
    int di = 0;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const uint16_t be = data[di++];
            if (static_cast<unsigned>(x) < kW && static_cast<unsigned>(y) < kH)
                s_fb[y * kW + x] = __builtin_bswap16(be);  // back to native-endian
        }
    }
}

int tft_width() {
    return kW;
}
int tft_height() {
    return kH;
}

}  // namespace pixfrog::ui::detail

// ── Emulator-facing accessors (outside the detail namespace) ─────────────────
const uint16_t* emu_fb_ptr() {
    return pixfrog::ui::detail::s_fb;
}
int emu_fb_width() {
    return pixfrog::ui::detail::tft_width();
}
int emu_fb_height() {
    return pixfrog::ui::detail::tft_height();
}
