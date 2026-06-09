// SSD1306 128×64 I2C OLED driver — text-only, page-oriented.
//
// Layout: 8 pages × 128 columns. Each page is 8 vertical pixels.
// A 21-column × 8-row character grid is overlaid (chars are 6-px wide
// including 1 trailing blank column = 21 chars × 6 px = 126 px ≤ 128).
//
// Architecture:
//   - oled_draw_text() rasterises chars into a local 8-page × 128-column
//     framebuffer (1 ko) and marks the row dirty.
//   - oled_flush() pushes only dirty pages over I2C (3 bytes command +
//     128 bytes data each). Idle: 0 bytes/I2C.

#include "ui_internal.h"

#include <cstring>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "font.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "OLED";

constexpr uint8_t kSsd1306Cols = 128;
constexpr uint8_t kPages       = 8;
constexpr uint8_t kTextCols    = 21;  // = 128 / kFontCellWidth (= 6), rounded down
constexpr uint8_t kTextRows    = 8;
constexpr int kI2cTimeoutMs    = 50;

// SSD1306 control-byte prefixes for I2C transactions.
constexpr uint8_t kPrefixCmdStream  = 0x00;  // Co=0, D/C=0 → next bytes = commands
constexpr uint8_t kPrefixDataStream = 0x40;  // Co=0, D/C=1 → next bytes = display data

// Init sequence (128×64, internal charge pump, horizontal addressing).
// clang-format off
constexpr uint8_t kInitCmds[] = {
    kPrefixCmdStream,
    0xAE,             // display off
    0xD5, 0x80,       // set display clock divide ratio / oscillator freq
    0xA8, 0x3F,       // multiplex ratio = 63 (1/64 duty)
    0xD3, 0x00,       // display offset 0
    0x40,             // start line 0
    0x8D, 0x14,       // charge pump enable
    0x20, 0x00,       // memory addressing mode: horizontal
    0xA1,             // segment remap (col 127 → SEG0)
    0xC8,             // COM scan dir reverse (top → bottom)
    0xDA, 0x12,       // COM pin hardware config (alternating)
    0x81, 0xCF,       // contrast
    0xD9, 0xF1,       // pre-charge
    0xDB, 0x40,       // VCOMH deselect level
    0xA4,             // resume from RAM (no entire-display-on)
    0xA6,             // normal display (not inverted)
    0x2E,             // deactivate scroll
    0xAF,             // display on
};
// clang-format on

i2c_master_dev_handle_t g_dev             = nullptr;
uint8_t g_fb[kPages][kSsd1306Cols]        = {};  // next-to-flush framebuffer
uint8_t g_fb_prev[kPages][kSsd1306Cols]   = {};  // last-flushed (for diff)
char g_text_buf[kTextRows][kTextCols + 1] = {};

bool send_cmds(const uint8_t* data, size_t len) {
    if (!g_dev) return false;
    esp_err_t err = i2c_master_transmit(g_dev, data, len, kI2cTimeoutMs);
    if (err != ESP_OK) ESP_LOGW(TAG, "i2c cmd tx failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

// Rasterise one text row's characters into the framebuffer page that
// row maps to (1 text row = 1 SSD1306 page = 1 byte per column).
void rasterise_row(uint8_t row) {
    uint8_t* page = g_fb[row];
    std::memset(page, 0, kSsd1306Cols);
    for (uint8_t tc = 0; tc < kTextCols; ++tc) {
        const char c = g_text_buf[row][tc];
        if (c == '\0') break;
        const uint8_t* alpha = font_alpha_for(c);
        const size_t base    = static_cast<size_t>(tc) * kFontCellWidth;
        // Threshold the AA coverage to 1bpp columns (bit r = row r, top=bit0).
        for (uint8_t gc = 0; gc < kFontWidth; ++gc) {
            if (base + gc >= kSsd1306Cols) break;
            uint8_t colbyte = 0;
            for (uint8_t r = 0; r < kFontHeight; ++r) {
                if (alpha[r * kFontCellWidth + gc] >= 128) colbyte |= static_cast<uint8_t>(1u << r);
            }
            page[base + gc] = colbyte;
        }
        // 1 blank column after each glyph (already zero-init'd)
    }
}

}  // namespace

bool oled_init(i2c_master_bus_handle_t bus, uint8_t addr) {
    if (!bus) {
        ESP_LOGE(TAG, "null I2C bus");
        return false;
    }
    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = 400'000;
    esp_err_t err           = i2c_master_bus_add_device(bus, &dev_cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_add_device: %s", esp_err_to_name(err));
        return false;
    }
    // The first transaction on a freshly created bus can NACK, and the panel's
    // charge pump needs a moment after power-on; poll until the address ACKs
    // (up to ~400 ms) before sending the init sequence.
    esp_err_t probe = ESP_FAIL;
    for (int i = 0; i < 20; ++i) {
        probe = i2c_master_probe(bus, addr, kI2cTimeoutMs);
        if (probe == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02X after retries: %s", addr, esp_err_to_name(probe));
        return false;
    }
    if (!send_cmds(kInitCmds, sizeof(kInitCmds))) {
        ESP_LOGE(TAG, "SSD1306 init sequence failed (no OLED attached?)");
        return false;
    }
    // SSD1306 GDDRAM is undefined at power-on. Force a full-page flush by
    // making the "last-flushed" buffer differ from the "next" buffer on
    // every byte; the diff in oled_flush() will then push all 8 pages.
    std::memset(g_fb_prev, 0xFF, sizeof(g_fb_prev));
    oled_clear();
    oled_flush();
    ESP_LOGI(TAG, "SSD1306 init OK at 0x%02X", addr);
    return true;
}

void oled_clear() {
    std::memset(g_fb, 0, sizeof(g_fb));
    std::memset(g_text_buf, 0, sizeof(g_text_buf));
}

void oled_draw_text(uint8_t row, uint8_t col, const char* str) {
    if (row >= kTextRows) return;
    for (uint8_t c = col; c < kTextCols && *str; ++c, ++str) {
        g_text_buf[row][c] = *str;
    }
    rasterise_row(row);
}

void oled_flush() {
    if (!g_dev) return;
    // Diff-based: only push pages whose rendered bytes differ from what's
    // currently on screen. At idle (no text change) zero bytes go over I2C.
    uint8_t page_cmds[4] = { kPrefixCmdStream, 0xB0, 0x00, 0x10 };
    uint8_t page_data[1 + kSsd1306Cols];
    page_data[0] = kPrefixDataStream;

    for (uint8_t p = 0; p < kPages; ++p) {
        if (std::memcmp(g_fb[p], g_fb_prev[p], kSsd1306Cols) == 0) continue;
        page_cmds[1] = static_cast<uint8_t>(0xB0 | p);
        if (!send_cmds(page_cmds, sizeof(page_cmds))) return;
        std::memcpy(page_data + 1, g_fb[p], kSsd1306Cols);
        esp_err_t err = i2c_master_transmit(g_dev, page_data, sizeof(page_data), kI2cTimeoutMs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "page %u tx failed: %s", p, esp_err_to_name(err));
            return;
        }
        std::memcpy(g_fb_prev[p], g_fb[p], kSsd1306Cols);
    }
}

}  // namespace pixfrog::ui::detail
