#include "font_5x8.h"
#include "ui_internal.h"

#include <algorithm>
#include <cstring>

namespace pixfrog::ui::detail {

namespace {

// Scan-line buffer: one row x max width (480 bytes)
static uint16_t s_row_buf[240];

// Text block buffer: max 240px wide x 24px tall = 11520 bytes (scale<=3)
static uint16_t s_text_buf[240 * 24];

// ST7789 expects big-endian RGB565; swap bytes since ESP32 is little-endian.
inline uint16_t to_hw(Color c) {
    return __builtin_bswap16(c.v);
}

}  // namespace

int canvas_width() {
    return tft_width();
}
int canvas_height() {
    return tft_height();
}

void canvas_clear(Color bg) {
    const uint16_t hw = to_hw(bg);
    for (int i = 0; i < tft_width(); i++)
        s_row_buf[i] = hw;
    for (int y = 0; y < tft_height(); y++)
        tft_draw_bitmap(0, y, tft_width(), y + 1, s_row_buf);
}

void canvas_fill_rect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    const int clamped_w = (w > 240) ? 240 : w;
    const uint16_t hw   = to_hw(c);
    for (int i = 0; i < clamped_w; i++)
        s_row_buf[i] = hw;
    for (int row = y; row < y + h; row++)
        tft_draw_bitmap(x, row, x + clamped_w, row + 1, s_row_buf);
}

void canvas_draw_text(int x, int y, const char* str, Color fg, Color bg, uint8_t scale) {
    if (!str || !*str) return;
    const int s  = (scale < 1) ? 1 : scale;
    const int cw = kFontCellWidth * s;
    const int ch = kFontHeight * s;
    int len      = 0;
    while (str[len])
        ++len;
    const int tw = len * cw;
    const int th = ch;

    if (tw <= 0 || tw > 240 || th > 24) return;

    const uint16_t hw_fg = to_hw(fg);
    const uint16_t hw_bg = to_hw(bg);
    for (int i = 0; i < tw * th; i++)
        s_text_buf[i] = hw_bg;

    for (int ci = 0; ci < len; ci++) {
        const uint8_t* glyph = font_glyph_for(str[ci]);
        const int cx         = ci * cw;
        for (int col = 0; col < kFontWidth; col++) {
            const uint8_t coldata = glyph[col];
            for (int row = 0; row < kFontHeight; row++) {
                const uint16_t pix = ((coldata >> row) & 1u) ? hw_fg : hw_bg;
                for (int sy = 0; sy < s; sy++) {
                    for (int sx = 0; sx < s; sx++) {
                        const int px             = cx + col * s + sx;
                        const int py             = row * s + sy;
                        s_text_buf[py * tw + px] = pix;
                    }
                }
            }
        }
    }

    tft_draw_bitmap(x, y, x + tw, y + th, s_text_buf);
}

void canvas_flush() {}  // direct-write: nothing to flush

}  // namespace pixfrog::ui::detail
