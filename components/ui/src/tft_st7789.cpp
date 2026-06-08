#include "ui_internal.h"

#include "driver/spi_master.h"
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
}  // namespace

bool tft_init(const TftConfig& cfg) {
    g_w = cfg.width;
    g_h = cfg.height;

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
    io_cfg.dc_gpio_num            = cfg.dc_gpio;
    io_cfg.cs_gpio_num            = cfg.cs_gpio;
    io_cfg.pclk_hz                = cfg.freq_hz;
    io_cfg.lcd_cmd_bits           = 8;
    io_cfg.lcd_param_bits         = 8;
    io_cfg.spi_mode               = 0;
    io_cfg.trans_queue_depth      = 10;
    io_cfg.flags.swap_color_bytes = 1;  // ST7789 big-endian colors

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
    esp_lcd_panel_invert_color(g_panel, true);
    esp_lcd_panel_mirror(g_panel, true, false);
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

}  // namespace pixfrog::ui::detail
