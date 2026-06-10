#include "ui_internal.h"

// TFT-only animated splash screen.
//
// Replays the Collecti'Frog logo animation from .github/pages/about.html: the
// frog surfaces from the water, the bands settle and the eyes blink. The frame
// strip is baked by tools/splashgen into splash_anim.cpp (1bpp masks); here we
// just blit frame[t] over the dark-green backdrop next to the wordmark.
//
// Done (returns true) when the strip finishes or on a click.

namespace pixfrog::ui::detail {

#ifdef CONFIG_PIXFROG_DISPLAY_TFT

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// Frog window: left of the wordmark, vertically centred.
constexpr int kFrogX = 10;

}  // namespace

bool splash_render(uint32_t t_ms, bool clicked) {
    const int count         = splash_anim_count();
    const uint32_t frame_ms = splash_anim_frame_ms();
    const uint32_t total    = static_cast<uint32_t>(count) * frame_ms;
    if (clicked || t_ms >= total) return true;

    const int fw    = splash_anim_w();
    const int fh    = splash_anim_h();
    const int frogY = (kH - fh) / 2;

    // Backdrop + wordmark, then the current frog frame. Repainted whole each
    // call, like render_home() — the UI has no back buffer.
    canvas_clear(color::FrogBg);
    constexpr int kScale = 3;  // scale 4 would exceed canvas_draw_text's 24px block
    const int wx         = kFrogX + fw + 12;
    canvas_draw_text(wx, kH / 2 - kFontHeight * kScale / 2 - 6, "pixfrog", color::Cream,
                     color::FrogBg, kScale);
    canvas_draw_text(wx, kH / 2 + kFontHeight * kScale / 2 + 2, "ARTNET . LED DRIVER",
                     color::SplashSub, color::FrogBg, 1);

    const int idx = static_cast<int>(t_ms / frame_ms);
    canvas_draw_mask(kFrogX, frogY, fw, fh, splash_anim_frame(idx), color::FrogLine, color::FrogBg);

    canvas_flush();
    return false;
}

#else

// OLED: a static logo splash — the frog mark above the "pixfrog" wordmark —
// held briefly, skippable with a click. Drawn straight into the framebuffer
// with oled_set_pixel; oled_flush is diff-based so the repeated repaint is free.

namespace {

constexpr int kW         = 128;
constexpr uint32_t kHold = 1800;  // ms

void blit_mask(int x0, int y0, int w, int h, const uint8_t* data) {
    const int stride = (w + 7) / 8;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((data[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1)
                oled_set_pixel(x0 + x, y0 + y, true);
}

// Draw text with the crisp 5x7 OLED font scaled up by `scale` (each ink pixel
// becomes a scale×scale block). Advance is kFontCellWidth so spacing matches.
void blit_text(int x0, int y0, const char* s, int scale) {
    int cx = x0;
    for (const char* p = s; *p; ++p) {
        const uint8_t* col = font_oled_for(*p);
        for (int c = 0; c < kFontWidth; ++c)
            for (int r = 0; r < 7; ++r)
                if ((col[c] >> r) & 1)
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx)
                            oled_set_pixel(cx + c * scale + sx, y0 + r * scale + sy, true);
        cx += kFontCellWidth * scale;
    }
}

}  // namespace

bool splash_render(uint32_t t_ms, bool clicked) {
    if (clicked || t_ms >= kHold) return true;

    oled_clear();
    const int fw = frog_oled_w(), fh = frog_oled_h();
    blit_mask((kW - fw) / 2, 0, fw, fh, frog_oled_data());

    constexpr int kScale = 2;
    constexpr int kChars = 7;  // "pixfrog"
    const int wordmark_w = kChars * kFontCellWidth * kScale;
    blit_text((kW - wordmark_w) / 2, fh + 3, "pixfrog", kScale);

    oled_flush();
    return false;
}

#endif

}  // namespace pixfrog::ui::detail
