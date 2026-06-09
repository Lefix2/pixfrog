// Internal types and APIs shared between ui.cpp, menu.cpp, canvas_*.cpp,
// oled_ssd1306.cpp / tft_st7789.cpp, encoder_seesaw.cpp.

#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "font.h"

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
constexpr Color HeaderBg{ 0x1083 };     // near-black header bar (#14121a)
constexpr Color HeaderLine{ 0x2928 };   // faint accent line under header
constexpr Color FrogBg{ 0x0842 };       // splash backdrop — near-black (#0c0a14)
constexpr Color FrogLine{ 0x7691 };     // splash frog ink — spring green (#70d18b)
constexpr Color Cream{ 0xF77B };        // splash / label text — warm white (#f4eedc)
constexpr Color SplashSub{ 0x8C14 };    // splash subtitle — muted blue-grey (#8a83a0)
constexpr Color Gold{ 0xFE2C };         // value text — warm gold (#f8c662)
constexpr Color CursorBg{ 0x3963 };     // warm dark amber cursor highlight (#3a2f1c)
constexpr Color BadgeGreen{ 0x7691 };   // channel badge — NRZ strips (#70d18b)
constexpr Color BadgePurple{ 0xACDC };  // channel badge — clocked SPI (#a89be0)
constexpr Color AltRowBg{ 0x18C3 };     // slightly lighter dark for alternating rows
}  // namespace color

// ── Display layout ────────────────────────────────────────────────────────────

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
constexpr uint8_t kDisplayScale = 2;
constexpr uint8_t kRows         = 8;   // (240 - 28) / 24 visible rows (landscape)
constexpr uint8_t kCols         = 26;  // 320 / (6*2)
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
void canvas_hline(int x, int y, int w, Color c);
void canvas_vline(int x, int y, int h, Color c);
void canvas_fill_round_rect(int x, int y, int w, int h, int r, Color c);
// mask: 1bpp MSB-first data; bg.v==0xFFFF means transparent (TFT only).
void canvas_draw_mask(int x, int y, int w, int h, const uint8_t* mask, Color fg,
                      Color bg = color::Black);
// Draw text at pixel (x,y). scale multiplies the 5x8 glyph.
// OLED impl: ignores color/scale, maps (x,y) -> (row=y/8, col=x/6).
void canvas_draw_text(int x, int y, const char* str, Color fg, Color bg = color::Black,
                      uint8_t scale = 1);
void canvas_flush();

// ── OLED low-level (used only by oled_ssd1306.cpp + canvas_oled.cpp) ─────────
bool oled_init(i2c_master_bus_handle_t bus, uint8_t addr);
void oled_clear();
void oled_draw_text(uint8_t row, uint8_t col, const char* str);
void oled_flush();

// ── TFT low-level (used only by tft_st7789.cpp + canvas_tft.cpp) ─────────────
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

// ── Encoder driver ────────────────────────────────────────────────────────────

bool encoder_init(i2c_master_bus_handle_t bus, uint8_t addr, int int_gpio);
Event encoder_poll();

// On-board NeoPixel feedback (Adafruit 4991). init() after encoder_init().
// set_active(false) breathes green on the status screen; set_active(true) holds
// full green in config and flash() blips yellow per action. tick() once per UI
// loop (~30 Hz) animates it.
void encoder_led_init();
void encoder_led_set_active(bool active);
void encoder_led_flash();
void encoder_led_tick();

// ── Menu state machine ────────────────────────────────────────────────────────

void menu_init();
void menu_dispatch(Event e);
void menu_render();
void menu_on_idle_timeout();
bool menu_is_home();  // true on the "état général" status screen

// ── Splash screen ─────────────────────────────────────────────────────────────
// Returns true when done (time elapsed or clicked). OLED: always true.
bool splash_render(uint32_t t_ms, bool clicked);

// ── Baked frog animation (TFT only) ─────────────────────────────────────────
// Generated by tools/splashgen from .github/pages/about.html into
// splash_anim.cpp. 1bpp MSB-first masks, stride = (w+7)/8.
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
int splash_anim_w();
int splash_anim_h();
int splash_anim_count();
uint32_t splash_anim_frame_ms();
const uint8_t* splash_anim_frame(int i);
#endif

#ifdef PIXFROG_EMULATOR
// Debug snapshot of the menu FSM for the emulator's agent API ("state" cmd).
// screen_name points to a static string; cursor/channel are the live indices.
void menu_debug_state(const char** screen_name, int* cursor, int* channel);
#endif

}  // namespace pixfrog::ui::detail
