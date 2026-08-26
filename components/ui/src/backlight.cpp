// Backlight policy — shared by the firmware UI task (ui.cpp) and the host
// emulator loop. Turns GlobalConfig (brightness + idle attenuation) into calls
// on the platform backend, and owns the live-preview override used while the
// level is being edited.

#include "config_store.h"
#include "ui_internal.h"

namespace pixfrog::ui::detail {

namespace {

bool g_on       = false;  // false until the first frame is on the panel
bool g_idle     = false;
bool g_preview  = false;
uint8_t g_level = 0;  // level currently driven on the backend

uint8_t target_level(bool idle) {
    const auto& g       = config::get_global();
    const uint8_t level = config::tft_brightness_pct(g);
    if (!idle) return level;
    const uint8_t dim = config::tft_idle_dim_pct(g);
    return static_cast<uint8_t>(level * (100 - dim) / 100);
}

void drive(uint8_t pct, bool fade) {
    if (pct == g_level) return;
    g_level = pct;
    backlight_backend_set(pct, fade);
}

}  // namespace

void backlight_on() {
    g_on = true;
    drive(target_level(g_idle), true);  // fade in — no hard flash at boot
}

void backlight_tick(uint32_t idle_ms) {
    if (!g_on || g_preview) return;
    // The delay is read every tick rather than latched at boot, so a change from
    // the menu, the web UI or the console takes effect without a reboot.
    const auto& g   = config::get_global();
    const bool idle = config::tft_dim_enabled(g) && idle_ms >= config::tft_dim_delay_s(g) * 1000u;
    const bool changed = (idle != g_idle);
    g_idle             = idle;
    // Idle transitions ramp; a level edited elsewhere (web, console) applies at
    // once so the change reads as a direct response.
    drive(target_level(idle), changed);
}

bool backlight_is_dimmed() {
    return g_idle;
}

void backlight_preview(uint8_t pct) {
    g_preview = true;
    if (g_on) drive(pct, false);
}

void backlight_preview_end() {
    g_preview = false;
    if (g_on) drive(target_level(g_idle), false);
}

}  // namespace pixfrog::ui::detail
