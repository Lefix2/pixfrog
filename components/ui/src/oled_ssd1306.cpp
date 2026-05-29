// Minimal SSD1306 128×64 I2C driver — text-only, page-oriented (no full
// frame buffer to save RAM). 8 rows × 21 cols of 6×8 font.
//
// STATUS: SKELETON. The text-render and I2C transaction code below
// describes the surface and a flush strategy, but the font ROM is not
// embedded yet and the I2C write helper is stubbed. The OLED will not
// display anything until those are added.

#include "ui_internal.h"

#include "esp_log.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "OLED";

constexpr uint8_t kOledCols = 21;
constexpr uint8_t kOledRows = 8;

int      g_i2c_port = 0;
uint8_t  g_addr     = 0x3C;
char     g_buf[kOledRows][kOledCols + 1] = {};
bool     g_dirty_row[kOledRows] = {};

}  // namespace

bool oled_init(int i2c_port, uint8_t addr) {
    g_i2c_port = i2c_port;
    g_addr     = addr;
    // TODO: send SSD1306 init sequence:
    //   AE 00 10 40 81 7F A1 C8 A6 A8 3F D3 00 D5 80 D9 22 DA 12 DB 30
    //   8D 14 20 00 AF
    // via i2c_master_transmit().
    ESP_LOGW(TAG, "init skeleton — no I2C bring-up yet");
    return true;
}

void oled_clear() {
    for (uint8_t r = 0; r < kOledRows; ++r) {
        for (uint8_t c = 0; c < kOledCols; ++c) g_buf[r][c] = ' ';
        g_buf[r][kOledCols] = '\0';
        g_dirty_row[r] = true;
    }
}

void oled_draw_text(uint8_t row, uint8_t col, const char* str) {
    if (row >= kOledRows) return;
    for (uint8_t c = col; c < kOledCols && *str; ++c, ++str) {
        g_buf[row][c] = *str;
    }
    g_dirty_row[row] = true;
}

void oled_flush() {
    // TODO: for each dirty row, set page address (0xB0+row), set column 0,
    // then transmit 128 bytes of font glyphs for the row.
    for (uint8_t r = 0; r < kOledRows; ++r) g_dirty_row[r] = false;
}

}  // namespace pixfrog::ui::detail
