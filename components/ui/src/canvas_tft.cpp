#include "font.h"
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pixfrog::ui::detail {

namespace {

// Scan-line buffer: one row x max width (landscape = 320px)
static uint16_t s_row_buf[320];

// Text block buffer: max 320px wide x 32px tall (large font at scale 4)
static uint16_t s_text_buf[320 * 32];

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

// Corner inset for a corner row: r minus the half-width of the circle at the
// row centre (dy + 0.5 from the circle centre), in integer maths:
// span = max dx with (2dx)² + (2dy+1)² ≤ (2r)².
static int round_rect_inset(int r, int dy) {
    const int d2 = (2 * dy + 1) * (2 * dy + 1);
    int span     = 0;
    while (4 * (span + 1) * (span + 1) + d2 <= 4 * r * r)
        ++span;
    return r - span;
}

void canvas_fill_round_rect(int x, int y, int w, int h, int r, Color c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        canvas_fill_rect(x, y, w, h, c);
        return;
    }
    const uint16_t hw = to_hw(c);
    for (int row = 0; row < h; ++row) {
        int x_off = 0;
        if (row < r) {
            x_off = round_rect_inset(r, r - 1 - row);
        } else if (row >= h - r) {
            x_off = round_rect_inset(r, row - (h - r));
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

// Forward declaration (defined below, shared with the AA shape fill).
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a);

// Anti-aliased rounded rectangle (w == h == 2r gives a circle). Edge pixels
// are blended toward `bg` — the colour already behind the shape — because the
// panel cannot be read back. Renders the whole block through s_text_buf, so
// it must be issued before any text that lands on top of it.
void canvas_fill_round_rect_aa(int x, int y, int w, int h, int r, Color fg, Color bg) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (w * h > static_cast<int>(sizeof(s_text_buf) / sizeof(s_text_buf[0]))) {
        canvas_fill_round_rect(x, y, w, h, r, fg);
        return;
    }
    const float half_w = w * 0.5f, half_h = h * 0.5f, rr = static_cast<float>(r);
    for (int py = 0; py < h; ++py) {
        const float qy = std::fabs(py + 0.5f - half_h) - (half_h - rr);
        for (int px = 0; px < w; ++px) {
            // Signed distance to the rounded box, sampled at the pixel centre;
            // coverage ramps over the one-pixel band around the contour.
            const float qx    = std::fabs(px + 0.5f - half_w) - (half_w - rr);
            const float ax    = qx > 0.0f ? qx : 0.0f;
            const float ay    = qy > 0.0f ? qy : 0.0f;
            const float inner = qx > qy ? qx : qy;
            const float dist  = std::sqrt(ax * ax + ay * ay) + (inner < 0.0f ? inner : 0.0f) - rr;
            float cov         = 0.5f - dist;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            const uint8_t a         = static_cast<uint8_t>(cov * 255.0f + 0.5f);
            s_text_buf[py * w + px] = __builtin_bswap16(blend565(fg.v, bg.v, a));
        }
    }
    tft_draw_bitmap(x, y, x + w, y + h, s_text_buf);
}

// Blend fg over bg by 8-bit coverage in RGB565 channel space.
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    if (a == 0) return bg;
    if (a == 255) return fg;
    const uint32_t fr = (fg >> 11) & 0x1F, fgr = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    const uint32_t br = (bg >> 11) & 0x1F, bgr = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    const uint32_t r = (fr * a + br * (255 - a)) / 255;
    const uint32_t g = (fgr * a + bgr * (255 - a)) / 255;
    const uint32_t b = (fb * a + bb * (255 - a)) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void canvas_draw_text(int x, int y, const char* str, Color fg, Color bg, uint8_t scale) {
    if (!str || !*str) return;
    int s = (scale < 1) ? 1 : scale;
    // Even scales swap in the natively rasterised 12×16 cell at half the scale:
    // identical advance/line metrics in small-cell units, but crisp glyphs
    // instead of pixel-doubled 6×8 ones.
    const bool large = (s % 2 == 0);
    const int cell_w = large ? kFontLargeCellWidth : kFontCellWidth;
    const int cell_h = large ? kFontLargeHeight : kFontHeight;
    if (large) s /= 2;
    const int cw = cell_w * s;
    const int ch = cell_h * s;
    int len      = 0;
    while (str[len])
        ++len;
    const int tw = len * cw;
    const int th = ch;

    if (tw <= 0 || tw > 320 || th > 32) return;

    // Blend in native RGB565, byte-swap once on store (to_hw handles the swap).
    const uint16_t nat_bg = bg.v;
    const uint16_t hw_bg  = to_hw(bg);
    for (int i = 0; i < tw * th; i++)
        s_text_buf[i] = hw_bg;

    for (int ci = 0; ci < len; ci++) {
        const uint8_t* glyph = large ? font_large_alpha_for(str[ci]) : font_alpha_for(str[ci]);
        const int cx         = ci * cw;
        for (int col = 0; col < cell_w; col++) {
            for (int row = 0; row < cell_h; row++) {
                const uint8_t a = glyph[row * cell_w + col];
                if (a == 0) continue;  // bg already written
                const uint16_t pix = __builtin_bswap16(blend565(fg.v, nat_bg, a));
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
