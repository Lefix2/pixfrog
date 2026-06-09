#include "font_5x8.h"
#include "ui_internal.h"

#include <algorithm>
#include <cstring>

namespace pixfrog::ui::detail {

namespace {

// Scan-line buffer: one row x max width (landscape = 320px)
static uint16_t s_row_buf[320];

// Text block buffer: max 320px wide x 24px tall
static uint16_t s_text_buf[320 * 24];

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
    const int w       = tft_width();
    for (int i = 0; i < w; i++)
        s_row_buf[i] = hw;
    for (int y = 0; y < tft_height(); y++)
        tft_draw_bitmap(0, y, w, y + 1, s_row_buf);
}

void canvas_fill_rect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    const int max_w     = tft_width();
    const int clamped_w = (w > max_w) ? max_w : w;
    const uint16_t hw   = to_hw(c);
    for (int i = 0; i < clamped_w; i++)
        s_row_buf[i] = hw;
    for (int row = y; row < y + h; row++)
        tft_draw_bitmap(x, row, x + clamped_w, row + 1, s_row_buf);
}

void canvas_hline(int x, int y, int w, Color c) {
    canvas_fill_rect(x, y, w, 1, c);
}

void canvas_vline(int x, int y, int h, Color c) {
    if (h <= 0) return;
    const uint16_t hw = to_hw(c);
    s_row_buf[0]      = hw;
    for (int row = y; row < y + h; row++)
        tft_draw_bitmap(x, row, x + 1, row + 1, s_row_buf);
}

void canvas_fill_round_rect(int x, int y, int w, int h, int r, Color c) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) {
        canvas_fill_rect(x, y, w, h, c);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const uint16_t hw = to_hw(c);
    for (int row = 0; row < h; ++row) {
        int x_off = 0;
        if (row < r) {
            const int dy = r - 1 - row;
            for (int dx = 0; dx <= r; ++dx) {
                if (dx * dx + dy * dy <= r * r) {
                    x_off = r - dx;
                    break;
                }
            }
        } else if (row >= h - r) {
            const int dy = row - (h - r);
            for (int dx = 0; dx <= r; ++dx) {
                if (dx * dx + dy * dy <= r * r) {
                    x_off = r - dx;
                    break;
                }
            }
        }
        const int row_w = w - 2 * x_off;
        if (row_w <= 0) continue;
        for (int i = 0; i < row_w && i < 320; ++i)
            s_row_buf[i] = hw;
        tft_draw_bitmap(x + x_off, y + row, x + x_off + row_w, y + row + 1, s_row_buf);
    }
}

// mask: 1bpp, row-major, MSB-first, stride = (w+7)/8 bytes.
// Pixels where bit=1 → fg, bit=0 → bg.  bg.v==0xFFFF means transparent (skip).
void canvas_draw_mask(int x, int y, int w, int h, const uint8_t* mask, Color fg, Color bg) {
    if (!mask || w <= 0 || h <= 0) return;
    const uint16_t hw_fg   = to_hw(fg);
    const uint16_t hw_bg   = to_hw(bg);
    const bool transparent = (bg.v == 0xFFFFu);
    const int stride       = (w + 7) / 8;
    const int disp_w       = tft_width();
    const int disp_h       = tft_height();
    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        if (py < 0 || py >= disp_h) continue;
        int col_start = 0, col_end = w;
        if (x < 0) col_start = -x;
        if (x + w > disp_w) col_end = disp_w - x;
        if (col_start >= col_end) continue;
        if (transparent) {
            // Write each fg pixel individually to avoid a full-row redraw.
            for (int col = col_start; col < col_end; ++col) {
                const int byte_idx = col / 8;
                const int bit_idx  = 7 - (col % 8);
                if ((mask[row * stride + byte_idx] >> bit_idx) & 1u) {
                    s_row_buf[0] = hw_fg;
                    tft_draw_bitmap(x + col, py, x + col + 1, py + 1, s_row_buf);
                }
            }
        } else {
            for (int col = col_start; col < col_end; ++col) {
                const int byte_idx         = col / 8;
                const int bit_idx          = 7 - (col % 8);
                const bool set             = (mask[row * stride + byte_idx] >> bit_idx) & 1u;
                s_row_buf[col - col_start] = set ? hw_fg : hw_bg;
            }
            tft_draw_bitmap(x + col_start, py, x + col_end, py + 1, s_row_buf);
        }
    }
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

    if (tw <= 0 || tw > 320 || th > 24) return;

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
