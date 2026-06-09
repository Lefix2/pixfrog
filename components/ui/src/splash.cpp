#include "ui_internal.h"

// TFT-only animated splash screen.
// Timeline (t_ms from first call):
//   0–500ms   : four green wave bands rise from the bottom
//   500–800ms : "pixfrog" wordmark appears
//   800–900ms : blink (black flash)
//   900–1200ms: steady logo + "booting…" hint
//   >1200ms   : done (returns true)
// Click at any point skips straight to done.

namespace pixfrog::ui::detail {

#ifdef CONFIG_PIXFROG_DISPLAY_TFT

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// Wave rise: returns the top-y of wave band `wi` at time t_ms.
// Each band rises from below the screen to its final position.
int wave_top(int wi, uint32_t t_ms) {
    const uint32_t start_ms = static_cast<uint32_t>(wi) * 80u;
    if (t_ms < start_ms) return kH;
    const uint32_t dt = t_ms - start_ms;

    const int target_top = kH - kH / 4 * (wi + 1);
    if (dt >= 350u) return target_top;
    return kH - static_cast<int>(static_cast<uint32_t>(kH / 4 * (wi + 1)) * dt / 350u);
}

constexpr Color kWave[4] = {
    Color{ 0x0240 },  // darkest green
    Color{ 0x0360 },
    Color{ 0x0480 },
    color::DarkGreen,
};

}  // namespace

bool splash_render(uint32_t t_ms, bool clicked) {
    if (clicked || t_ms >= 1200u) return true;

    canvas_clear(color::Black);

    // Draw 4 wave bands from bottom layer to top layer.
    // Higher wi = topmost visible band.
    for (int wi = 0; wi < 4; ++wi) {
        const int top = wave_top(wi, t_ms);
        if (top >= kH) continue;
        canvas_fill_rect(0, top, kW, kH - top, kWave[wi]);
    }

    // Wordmark (appears at 500ms, blinks at 800–900ms)
    if (t_ms >= 500u) {
        const bool blink_off = (t_ms >= 800u && t_ms < 900u);
        if (!blink_off) {
            constexpr int kScale = 4;
            const char* wordmark = "pixfrog";
            const int text_w     = 7 * kFontCellWidth * kScale;
            const int tx         = (kW - text_w) / 2;
            const int ty         = kH / 2 - kFontHeight * kScale / 2 - 8;
            canvas_fill_round_rect(tx - 10, ty - 8, text_w + 20, kFontHeight * kScale + 16, 8,
                                   Color{ 0x0100 });
            canvas_draw_text(tx, ty, wordmark, color::White, Color{ 0x0100 }, kScale);
        }
    }

    if (t_ms >= 900u) {
        canvas_draw_text(4, kH - 10, "booting...", color::DarkGray, color::Black, 1);
    }

    canvas_flush();
    return false;
}

#else

bool splash_render(uint32_t /*t_ms*/, bool /*clicked*/) {
    return true;
}

#endif

}  // namespace pixfrog::ui::detail
