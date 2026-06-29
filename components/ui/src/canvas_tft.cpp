#include "font.h"
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pixfrog::ui::detail {

namespace {

// Off-screen framebuffer (double-buffered). Every canvas primitive renders
// into s_back; canvas_flush() pushes only the scan-lines that actually changed
// since the last flush (true partial refresh) — so a static screen costs no
// SPI traffic and there is never an all-black intermediate on the panel (the
// cause of the visible "wipe" of the old direct-write path). Both buffers are
// frame-buffer-sized → allocated in PSRAM via tft_fb_alloc (AGENT.md: no
// frame-buffer-sized SRAM). Pixels are stored big-endian (ST7789 wire order),
// the same value tft_draw_bitmap expects.
uint16_t* s_back   = nullptr;  // current frame being composed
uint16_t* s_shadow = nullptr;  // last frame pushed to the panel
int s_W            = 0;
int s_H            = 0;
bool s_force_full  = true;  // first flush pushes everything

// Per-band staging buffer (internal RAM, DMA-safe): the flush copies each
// changed band here before tft_draw_bitmap, so the panel DMA never sources
// from PSRAM. Sized to the widest supported panel × up to 32 rows.
constexpr int kBandRows = 32;
#ifdef CONFIG_PIXFROG_DISPLAY_NV3007
constexpr int kMaxWidth = 428;  // NV3007 rotated 90° → 428px wide
#else
constexpr int kMaxWidth = 320;
#endif
static uint16_t s_band_buf[kMaxWidth * kBandRows];

// ST7789 expects big-endian RGB565; swap bytes since ESP32 is little-endian.
inline uint16_t to_hw(Color c) {
    return __builtin_bswap16(c.v);
}

bool ensure_fb() {
    if (s_back) return true;
    s_W = tft_width();
    s_H = tft_height();
    if (s_W <= 0 || s_H <= 0 || s_W > kMaxWidth) return false;
    const unsigned long n = static_cast<unsigned long>(s_W) * s_H;
    s_back                = static_cast<uint16_t*>(tft_fb_alloc(n * sizeof(uint16_t)));
    s_shadow              = static_cast<uint16_t*>(tft_fb_alloc(n * sizeof(uint16_t)));
    if (!s_back || !s_shadow) {
        s_back = s_shadow = nullptr;
        return false;
    }
    std::memset(s_back, 0, n * sizeof(uint16_t));
    std::memset(s_shadow, 0, n * sizeof(uint16_t));
    s_force_full = true;
    return true;
}

inline void fb_hspan(int x, int y, int w, uint16_t hw) {
    if (y < 0 || y >= s_H || w <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w;
    if (x1 > s_W) x1 = s_W;
    if (x1 <= x0) return;
    uint16_t* p = &s_back[static_cast<long>(y) * s_W + x0];
    for (int i = 0; i < x1 - x0; ++i)
        p[i] = hw;
}

inline void fb_px(int x, int y, uint16_t hw) {
    if (static_cast<unsigned>(x) < static_cast<unsigned>(s_W) &&
        static_cast<unsigned>(y) < static_cast<unsigned>(s_H))
        s_back[static_cast<long>(y) * s_W + x] = hw;
}

}  // namespace

int canvas_width() {
    return tft_width();
}
int canvas_height() {
    return tft_height();
}

void canvas_clear(Color bg) {
    if (!ensure_fb()) return;
    const uint16_t hw = to_hw(bg);
    const long n      = static_cast<long>(s_W) * s_H;
    for (long i = 0; i < n; ++i)
        s_back[i] = hw;
}

void canvas_fill_rect(int x, int y, int w, int h, Color c) {
    if (!ensure_fb() || w <= 0 || h <= 0) return;
    const uint16_t hw = to_hw(c);
    for (int row = y; row < y + h; ++row)
        fb_hspan(x, row, w, hw);
}

void canvas_hline(int x, int y, int w, Color c) {
    canvas_fill_rect(x, y, w, 1, c);
}

void canvas_vline(int x, int y, int h, Color c) {
    if (!ensure_fb() || h <= 0) return;
    const uint16_t hw = to_hw(c);
    for (int row = y; row < y + h; ++row)
        fb_px(x, row, hw);
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
    if (!ensure_fb() || w <= 0 || h <= 0) return;
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
        fb_hspan(x + x_off, y + row, row_w, hw);
    }
}

// mask: 1bpp, row-major, MSB-first, stride = (w+7)/8 bytes.
// Pixels where bit=1 → fg, bit=0 → bg.  bg.v==0xFFFF means transparent (skip).
void canvas_draw_mask(int x, int y, int w, int h, const uint8_t* mask, Color fg, Color bg) {
    if (!ensure_fb() || !mask || w <= 0 || h <= 0) return;
    const uint16_t hw_fg   = to_hw(fg);
    const uint16_t hw_bg   = to_hw(bg);
    const bool transparent = (bg.v == 0xFFFFu);
    const int stride       = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        for (int col = 0; col < w; ++col) {
            const int byte_idx = col / 8;
            const int bit_idx  = 7 - (col % 8);
            const bool set     = (mask[row * stride + byte_idx] >> bit_idx) & 1u;
            if (set)
                fb_px(x + col, py, hw_fg);
            else if (!transparent)
                fb_px(x + col, py, hw_bg);
        }
    }
}

// Forward declaration (defined below, shared with the AA shape fill).
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a);

// Anti-aliased rounded rectangle (w == h == 2r gives a circle). Edge pixels are
// blended against whatever is already in the framebuffer behind the shape, so
// the contour stays clean over any background (no flat-colour corner fringe —
// the cause of the "irregular" look). `bg` is unused (kept for API symmetry).
void canvas_fill_round_rect_aa(int x, int y, int w, int h, int r, Color fg, Color bg) {
    (void)bg;
    if (!ensure_fb() || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const float half_w = w * 0.5f, half_h = h * 0.5f, rr = static_cast<float>(r);
    for (int py = 0; py < h; ++py) {
        const int dy = y + py;
        if (static_cast<unsigned>(dy) >= static_cast<unsigned>(s_H)) continue;
        const float qy = std::fabs(py + 0.5f - half_h) - (half_h - rr);
        for (int px = 0; px < w; ++px) {
            const int dx = x + px;
            if (static_cast<unsigned>(dx) >= static_cast<unsigned>(s_W)) continue;
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
            const uint8_t a = static_cast<uint8_t>(cov * 255.0f + 0.5f);
            if (a == 0) continue;  // fully outside → leave the background pixel
            uint16_t& dst = s_back[static_cast<long>(dy) * s_W + dx];
            dst = __builtin_bswap16(a == 255 ? fg.v : blend565(fg.v, __builtin_bswap16(dst), a));
        }
    }
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
    if (!ensure_fb() || !str || !*str) return;
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
    if (tw <= 0) return;

    const bool transparent = (bg.v == 0xFFFFu);
    const uint16_t nat_bg  = bg.v;
    if (!transparent) {
        // Opaque background: fill the whole block first.
        const uint16_t hw_bg = to_hw(bg);
        for (int row = 0; row < th; ++row)
            fb_hspan(x, y + row, tw, hw_bg);
    }

    // Composite one ink pixel: over the flat bg (opaque) or over whatever is
    // already in the framebuffer (transparent) so glyphs sit on any shape.
    auto put = [&](int X, int Y, uint8_t a) {
        if (static_cast<unsigned>(X) >= static_cast<unsigned>(s_W) ||
            static_cast<unsigned>(Y) >= static_cast<unsigned>(s_H))
            return;
        uint16_t& d         = s_back[static_cast<long>(Y) * s_W + X];
        const uint16_t bgnt = transparent ? __builtin_bswap16(d) : nat_bg;
        d                   = __builtin_bswap16(a == 255 ? fg.v : blend565(fg.v, bgnt, a));
    };

    for (int ci = 0; ci < len; ci++) {
        const uint8_t* glyph = large ? font_large_alpha_for(str[ci]) : font_alpha_for(str[ci]);
        const int cx         = ci * cw;
        for (int col = 0; col < cell_w; col++) {
            for (int row = 0; row < cell_h; row++) {
                const uint8_t a = glyph[row * cell_w + col];
                if (a == 0) continue;  // bg already written (opaque) / left as-is (transparent)
                for (int sy = 0; sy < s; sy++)
                    for (int sx = 0; sx < s; sx++)
                        put(x + cx + col * s + sx, y + row * s + sy, a);
            }
        }
    }
}

int canvas_text_xl_width(const char* str) {
    int n = 0;
    if (str)
        while (str[n])
            ++n;
    return n * kFontXLCellWidth;
}

// Crisp 18×24 wordmark, rendered 1:1 from the native XL cell (no upscaling).
void canvas_draw_text_xl(int x, int y, const char* str, Color fg, Color bg) {
    if (!ensure_fb() || !str || !*str) return;
    const bool transparent = (bg.v == 0xFFFFu);
    const uint16_t nat_bg  = bg.v;
    const int cw = kFontXLCellWidth, ch = kFontXLHeight;
    int len = 0;
    while (str[len])
        ++len;
    if (!transparent) {
        const uint16_t hw_bg = to_hw(bg);
        for (int row = 0; row < ch; ++row)
            fb_hspan(x, y + row, len * cw, hw_bg);
    }
    auto put = [&](int X, int Y, uint8_t a) {
        if (static_cast<unsigned>(X) >= static_cast<unsigned>(s_W) ||
            static_cast<unsigned>(Y) >= static_cast<unsigned>(s_H))
            return;
        uint16_t& d         = s_back[static_cast<long>(Y) * s_W + X];
        const uint16_t bgnt = transparent ? __builtin_bswap16(d) : nat_bg;
        d                   = __builtin_bswap16(a == 255 ? fg.v : blend565(fg.v, bgnt, a));
    };
    for (int ci = 0; ci < len; ++ci) {
        const uint8_t* glyph = font_xl_alpha_for(str[ci]);
        const int cx         = ci * cw;
        for (int col = 0; col < cw; ++col)
            for (int row = 0; row < ch; ++row) {
                const uint8_t a = glyph[row * cw + col];
                if (a != 0) put(x + cx + col, y + row, a);
            }
    }
}

// Push every scan-line that differs from the last flush; identical lines (the
// common idle case) cost nothing. Changed lines are coalesced into bands of up
// to kBandRows and staged through the internal s_band_buf before the panel DMA.
void canvas_flush() {
    if (!ensure_fb()) return;
    const int rowbytes = s_W * static_cast<int>(sizeof(uint16_t));
    int y              = 0;
    while (y < s_H) {
        const uint16_t* fb_row = &s_back[static_cast<long>(y) * s_W];
        const uint16_t* sh_row = &s_shadow[static_cast<long>(y) * s_W];
        if (!s_force_full && std::memcmp(fb_row, sh_row, rowbytes) == 0) {
            ++y;
            continue;
        }
        const int y0 = y;
        int rows     = 0;
        while (y < s_H && rows < kBandRows) {
            const uint16_t* fr = &s_back[static_cast<long>(y) * s_W];
            const uint16_t* sr = &s_shadow[static_cast<long>(y) * s_W];
            if (!s_force_full && std::memcmp(fr, sr, rowbytes) == 0) break;
            ++y;
            ++rows;
        }
        const long span = static_cast<long>(rows) * s_W;
        std::memcpy(s_band_buf, &s_back[static_cast<long>(y0) * s_W], span * sizeof(uint16_t));
        tft_draw_bitmap(0, y0, s_W, y0 + rows, s_band_buf);
        std::memcpy(&s_shadow[static_cast<long>(y0) * s_W], &s_back[static_cast<long>(y0) * s_W],
                    span * sizeof(uint16_t));
    }
    s_force_full = false;
}

}  // namespace pixfrog::ui::detail
