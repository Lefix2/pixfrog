// Internal types and APIs shared between ui.cpp, menu.cpp, oled_ssd1306.cpp,
// encoder_seesaw.cpp.

#pragma once

#include <stdint.h>

namespace pixfrog::ui::detail {

enum class Event : uint8_t {
    None,
    RotateLeft,
    RotateRight,
    Click,
};

// ── OLED driver (tiny SSD1306 subset) ──────────────────────────────────────

bool oled_init(int i2c_port, uint8_t addr);
void oled_clear();
void oled_draw_text(uint8_t row, uint8_t col, const char* str);
void oled_flush();

// ── Encoder driver (seesaw subset) ─────────────────────────────────────────

bool encoder_init(int i2c_port, uint8_t addr, int int_gpio);
// Returns the next pending Event, draining one rotation tick or button press.
// Returns Event::None if the seesaw has nothing.
Event encoder_poll();

// ── Menu state machine ─────────────────────────────────────────────────────

void menu_init();
void menu_dispatch(Event e);
// Force a render to the OLED back-buffer; does NOT flush.
void menu_render();
// True if the menu wants the OLED to redraw on the next tick.
bool menu_is_dirty();
void menu_clear_dirty();
// Signal an idle timeout (UI returns HOME).
void menu_on_idle_timeout();

}  // namespace pixfrog::ui::detail
