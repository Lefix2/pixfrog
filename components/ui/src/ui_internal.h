// Internal types and APIs shared between ui.cpp, menu.cpp, oled_ssd1306.cpp,
// encoder_seesaw.cpp.

#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"

namespace pixfrog::ui::detail {

enum class Event : uint8_t {
    None,
    RotateLeft,
    RotateRight,
    Click,
};

// ── OLED driver (tiny SSD1306 subset) ──────────────────────────────────────

bool oled_init(i2c_master_bus_handle_t bus, uint8_t addr);
void oled_clear();
void oled_draw_text(uint8_t row, uint8_t col, const char* str);
void oled_flush();

// ── Encoder driver (seesaw subset) ─────────────────────────────────────────

bool encoder_init(i2c_master_bus_handle_t bus, uint8_t addr, int int_gpio);
// Returns the next pending Event, draining one rotation tick or button press.
// Returns Event::None if the seesaw has nothing.
Event encoder_poll();

// ── Menu state machine ─────────────────────────────────────────────────────

void menu_init();
void menu_dispatch(Event e);
// Render the current screen to the OLED back-buffer (no I2C transfer).
// Idempotent and cheap: rasterising the same content twice produces the
// same bytes, and oled_flush() then sends 0 bytes over I2C.
void menu_render();
// Signal an idle timeout (UI returns HOME).
void menu_on_idle_timeout();

}  // namespace pixfrog::ui::detail
