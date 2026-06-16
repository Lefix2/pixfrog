#include "ui_internal.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

namespace pixfrog::ui::detail {

namespace {
constexpr const char* TAG      = "TFT";
esp_lcd_panel_handle_t g_panel = nullptr;
int g_w                        = 0;
int g_h                        = 0;
int g_bl_gpio                  = -1;
}  // namespace

bool tft_init(const TftConfig& cfg) {
    g_w = cfg.width;
    g_h = cfg.height;

    // Backlight: configure off now, raised on after the first frame (ui.cpp).
    g_bl_gpio = cfg.backlight_gpio;
    if (g_bl_gpio >= 0) {
        gpio_config_t bl{};
        bl.pin_bit_mask = 1ULL << g_bl_gpio;
        bl.mode         = GPIO_MODE_OUTPUT;
        gpio_config(&bl);
        gpio_set_level(static_cast<gpio_num_t>(g_bl_gpio), 0);
    }

    spi_bus_config_t bus{};
    bus.mosi_io_num     = cfg.mosi_gpio;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = cfg.clk_gpio;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = cfg.width * 40 * 2;  // 40 scan-lines buffer

    if (spi_bus_initialize(static_cast<spi_host_device_t>(cfg.spi_host), &bus, SPI_DMA_CH_AUTO) !=
        ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.dc_gpio_num       = cfg.dc_gpio;
    io_cfg.cs_gpio_num       = cfg.cs_gpio;
    io_cfg.pclk_hz           = cfg.freq_hz;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.spi_mode          = 0;
    io_cfg.trans_queue_depth = 10;
    // swap_color_bytes removed in IDF v5.5; byte-swap via __builtin_bswap16 in canvas layer

    if (esp_lcd_new_panel_io_spi(static_cast<spi_host_device_t>(cfg.spi_host), &io_cfg, &io) !=
        ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi failed");
        return false;
    }

    esp_lcd_panel_dev_config_t pcfg{};
    pcfg.reset_gpio_num = cfg.rst_gpio;
    pcfg.color_space    = ESP_LCD_COLOR_SPACE_RGB;
    pcfg.bits_per_pixel = 16;

    if (esp_lcd_new_panel_st7789(io, &pcfg, &g_panel) != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789 failed");
        return false;
    }

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, false);
    esp_lcd_panel_swap_xy(g_panel, true);  // portrait → landscape (320×240)
    esp_lcd_panel_mirror(g_panel, true, false);

    // Paint GRAM black before enabling the display, else the panel's random
    // power-on contents flash (white) until the first UI frame is pushed.
    {
        static uint16_t zeros[320 * 16] = { 0 };  // 0x0000 = black, byte-order agnostic
        const int w = cfg.width, h = cfg.height;
        for (int y = 0; y < h; y += 16) {
            const int band = (y + 16 <= h) ? 16 : (h - y);
            esp_lcd_panel_draw_bitmap(g_panel, 0, y, w, y + band, zeros);
        }
    }
    esp_lcd_panel_disp_on_off(g_panel, true);

    ESP_LOGI(TAG, "ST7789 %dx%d ready", cfg.width, cfg.height);
    return true;
}

void tft_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t* data) {
    esp_lcd_panel_draw_bitmap(g_panel, x1, y1, x2, y2, data);
}

int tft_width() {
    return g_w;
}
int tft_height() {
    return g_h;
}

void tft_backlight(bool on) {
    if (g_bl_gpio >= 0) gpio_set_level(static_cast<gpio_num_t>(g_bl_gpio), on ? 1 : 0);
}

void* tft_fb_alloc(unsigned long bytes) {
    // Frame-buffer-sized → PSRAM (SRAM is reserved; see AGENT.md). The flush
    // path stages each band through an internal buffer, so the panel DMA never
    // sources from PSRAM directly.
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace pixfrog::ui::detail
