// Internal types and APIs shared between ui.cpp, menu.cpp, canvas_*.cpp,
// oled_ssd1306.cpp / tft_st7789.cpp, encoder_seesaw.cpp.

#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "font_5x8.h"

namespace pixfrog::ui::detail {

// ── Input events ──────────────────────────────────────────────────────────────

enum class Event : uint8_t { None, RotateLeft, RotateRight, Click };

// ── Color (RGB565) ─────────────────────────────────────────────────────────────
// On OLED: any non-Black fg = pixel ON. bg is ignored.

struct Color {
    uint16_t v;
    constexpr explicit Color(uint16_t rgb565) : v(rgb565) {}
    bool operator==(Color o) const { return v == o.v; }
    bool operator!=(Color o) const { return v != o.v; }
};

namespace color {
constexpr Color Black{ 0x0000 };
constexpr Color White{ 0xFFFF };
constexpr Color Green{ 0x07E0 };
constexpr Color Red{ 0xF800 };
constexpr Color Yellow{ 0xFFE0 };
constexpr Color Cyan{ 0x07FF };
constexpr Color Orange{ 0xFD20 };
constexpr Color DarkGreen{ 0x0380 };
constexpr Color DarkGray{ 0x39E7 };
constexpr Color LightGray{ 0xC618 };
constexpr Color DarkBlue{ 0x000F };
constexpr Color HeaderBg{ 0x0340 };  // dark teal header
constexpr Color CursorBg{ 0x0014 };  // dark blue cursor highlight
constexpr Color AltRowBg{ 0x18C3 };  // slightly lighter dark for alternating rows
}  // namespace color

// ── Display layout ────────────────────────────────────────────────────────────

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
constexpr uint8_t kDisplayScale = 2;
constexpr uint8_t kRows         = 13;  // (320 - 28) / 22 visible rows
constexpr uint8_t kCols         = 20;  // 240 / (6*2)
#else
constexpr uint8_t kDisplayScale = 1;
constexpr uint8_t kRows         = 8;
constexpr uint8_t kCols         = 21;
#endif

// ── Canvas API ────────────────────────────────────────────────────────────────
// Implemented by canvas_oled.cpp (OLED mode) or canvas_tft.cpp (TFT mode).

int canvas_width();
int canvas_height();
void canvas_clear(Color bg = color::Black);
void canvas_fill_rect(int x, int y, int w, int h, Color c);
// Draw text at pixel (x,y). scale multiplies the 5x8 glyph.
// OLED impl: ignores color/scale, maps (x,y) -> (row=y/8, col=x/6).
void canvas_draw_text(int x, int y, const char* str, Color fg, Color bg = color::Black,
                      uint8_t scale = 1);
void canvas_flush();

// ── OLED low-level (used only by oled_ssd1306.cpp + canvas_oled.cpp) ─────────
#ifdef CONFIG_PIXFROG_DISPLAY_OLED
bool oled_init(i2c_master_bus_handle_t bus, uint8_t addr);
void oled_clear();
void oled_draw_text(uint8_t row, uint8_t col, const char* str);
void oled_flush();
#endif

// ── TFT low-level (used only by tft_st7789.cpp + canvas_tft.cpp) ─────────────
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
struct TftConfig {
    int spi_host;
    int clk_gpio, mosi_gpio, cs_gpio, dc_gpio, rst_gpio;
    uint32_t freq_hz;
    int width, height;
};
bool tft_init(const TftConfig& cfg);
void tft_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t* data);
int tft_width();
int tft_height();
#endif

// ── Encoder driver ────────────────────────────────────────────────────────────

bool encoder_init(i2c_master_bus_handle_t bus, uint8_t addr, int int_gpio);
Event encoder_poll();

// ── Menu state machine ────────────────────────────────────────────────────────

void menu_init();
void menu_dispatch(Event e);
void menu_render();
void menu_on_idle_timeout();

}  // namespace pixfrog::ui::detail
