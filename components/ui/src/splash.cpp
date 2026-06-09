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
                     color::DarkGray, color::FrogBg, 1);

    const int idx = static_cast<int>(t_ms / frame_ms);
    canvas_draw_mask(kFrogX, frogY, fw, fh, splash_anim_frame(idx), color::FrogLine, color::FrogBg);

    canvas_flush();
    return false;
}

#else

bool splash_render(uint32_t /*t_ms*/, bool /*clicked*/) {
    return true;
}

#endif

}  // namespace pixfrog::ui::detail
