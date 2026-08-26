// NV3007 TFT driver — 2.79" bar panel, portrait 142×428 glass driven as a
// 428×142 landscape canvas.
//
// The NV3007 reference code ("NV3007+IVO2.66" vendor package, mirrored in
// lvgl's lv_nv3007.c) never programs MADCTL: the scan direction is the
// hardware default and orientation is the host's problem. So the 90° rotation
// is done here in software — each landscape band from canvas_flush() is
// transposed into a portrait GRAM window. CONFIG_PIXFROG_NV3007_ROT180 picks
// the other 90° direction for modules mounted upside down.

#include "ui_internal.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "TFT";

esp_lcd_panel_io_handle_t g_io = nullptr;
SemaphoreHandle_t g_tx_done    = nullptr;
int g_w                        = 0;  // landscape canvas size (428×142)
int g_h                        = 0;

constexpr int kNativeW = 142;  // panel columns (sources)
constexpr int kNativeH = 428;  // panel rows (gates)
// The 168-source controller drives this 142-column glass starting at source
// 12 (LCD_X_OFFSET in the vendor reference code).
constexpr int kGramXOffset = 0x0C;

// Landscape rows per transposed chunk. canvas_flush() sends bands of ≤ 32
// rows; the chunk loop in tft_draw_bitmap keeps larger callers safe too.
constexpr int kXposeRows = 32;
// Staging for one transposed chunk (internal RAM, DMA-safe): ≤ kXposeRows
// portrait columns × the full 428-row native height. Zero-initialised (.bss),
// which the GRAM black-fill at init relies on.
uint16_t s_xpose[kXposeRows * kNativeH];

// Vendor init sequence, flat {cmd, len, params…} stream. Register meanings are
// undocumented (no public NV3007 datasheet); the values are the vendor's and
// must be replayed verbatim.
// clang-format off
constexpr uint8_t kInitSeq[] = {
    // 0xFF is the page-select / command-set unlock. 0xA5 opens the extended
    // page that every 0x50..0xF9 register below lives on; without it the whole
    // block is written into the void and the panel runs on its defaults.
    0xff, 1, 0xa5,
    0x9a, 1, 0x08,
    0x9b, 1, 0x08,
    0x9c, 1, 0xb0,
    0x9d, 1, 0x16,
    0x9e, 1, 0xc4,
    0x8f, 2, 0x55, 0x04,
    0x84, 1, 0x90,
    0x83, 1, 0x7b,
    0x85, 1, 0x33,
    0x60, 1, 0x00,
    0x70, 1, 0x00,
    0x61, 1, 0x02,
    0x71, 1, 0x02,
    0x62, 1, 0x04,
    0x72, 1, 0x04,
    0x6c, 1, 0x29,
    0x7c, 1, 0x29,
    0x6d, 1, 0x31,
    0x7d, 1, 0x31,
    0x6e, 1, 0x0f,
    0x7e, 1, 0x0f,
    0x66, 1, 0x21,
    0x76, 1, 0x21,
    0x68, 1, 0x3a,
    0x78, 1, 0x3a,
    0x63, 1, 0x07,
    0x73, 1, 0x07,
    0x64, 1, 0x05,
    0x74, 1, 0x05,
    0x65, 1, 0x02,
    0x75, 1, 0x02,
    0x67, 1, 0x23,
    0x77, 1, 0x23,
    0x69, 1, 0x08,
    0x79, 1, 0x08,
    0x6a, 1, 0x13,
    0x7a, 1, 0x13,
    0x6b, 1, 0x13,
    0x7b, 1, 0x13,
    0x6f, 1, 0x00,
    0x7f, 1, 0x00,
    0x50, 1, 0x00,
    0x52, 1, 0xd6,
    0x53, 1, 0x08,
    0x54, 1, 0x08,
    0x55, 1, 0x1e,
    0x56, 1, 0x1c,
    0xa0, 3, 0x2b, 0x24, 0x00,
    0xa1, 1, 0x87,
    0xa2, 1, 0x86,
    0xa5, 1, 0x00,
    0xa6, 1, 0x00,
    0xa7, 1, 0x00,
    0xa8, 1, 0x36,
    0xa9, 1, 0x7e,
    0xaa, 1, 0x7e,
    0xb9, 1, 0x85,
    0xba, 1, 0x84,
    0xbb, 1, 0x83,
    0xbc, 1, 0x82,
    0xbd, 1, 0x81,
    0xbe, 1, 0x80,
    0xbf, 1, 0x01,
    0xc0, 1, 0x02,
    0xc1, 1, 0x00,
    0xc2, 1, 0x00,
    0xc3, 1, 0x00,
    0xc4, 1, 0x33,
    0xc5, 1, 0x7e,
    0xc6, 1, 0x7e,
    0xc8, 2, 0x33, 0x33,
    0xc9, 1, 0x68,
    0xca, 1, 0x69,
    0xcb, 1, 0x6a,
    0xcc, 1, 0x6b,
    0xcd, 2, 0x33, 0x33,
    0xce, 1, 0x6c,
    0xcf, 1, 0x6d,
    0xd0, 1, 0x6e,
    0xd1, 1, 0x6f,
    0xab, 2, 0x03, 0x67,
    0xac, 2, 0x03, 0x6b,
    0xad, 2, 0x03, 0x68,
    0xae, 2, 0x03, 0x6c,
    0xb3, 1, 0x00,
    0xb4, 1, 0x00,
    0xb5, 1, 0x00,
    0xb6, 1, 0x32,
    0xb7, 1, 0x7e,
    0xb8, 1, 0x7e,
    0xe0, 1, 0x00,
    0xe1, 2, 0x03, 0x0f,
    0xe2, 1, 0x04,
    0xe3, 1, 0x01,
    0xe4, 1, 0x0e,
    0xe5, 1, 0x01,
    0xe6, 1, 0x19,
    0xe7, 1, 0x10,
    0xe8, 1, 0x10,
    0xea, 1, 0x12,
    0xeb, 1, 0xd0,
    0xec, 1, 0x04,
    0xed, 1, 0x07,
    0xee, 1, 0x07,
    0xef, 1, 0x09,
    0xf0, 1, 0xd0,
    0xf1, 1, 0x0e,
    0xf9, 1, 0x17,
    0xf2, 4, 0x2c, 0x1b, 0x0b, 0x20,
    0xe9, 1, 0x29,
    0xec, 1, 0x04,
    0x35, 1, 0x00,  // TE on
    0x44, 2, 0x00, 0x10,
    0x46, 1, 0x10,
    // Back to page 0 before the standard DCS commands: COLMOD on the extended
    // page would leave the controller at its 18 bit/pixel reset default (06h),
    // which reads a 2-byte-per-pixel stream as 3-byte pixels.
    0xff, 1, 0x00,
    0x3a, 1, 0x05,  // COLMOD: 16-bit RGB565
};
// clang-format on

bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t /*io*/,
                                   esp_lcd_panel_io_event_data_t* /*edata*/, void* /*ctx*/) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_tx_done, &hp);
    return hp == pdTRUE;
}

// Native portrait window, inclusive coordinates.
void set_window(int xs, int xe, int ys, int ye) {
    xs                  += kGramXOffset;
    xe                  += kGramXOffset;
    const uint8_t ca[4]  = { static_cast<uint8_t>(xs >> 8), static_cast<uint8_t>(xs),
                             static_cast<uint8_t>(xe >> 8), static_cast<uint8_t>(xe) };
    const uint8_t ra[4]  = { static_cast<uint8_t>(ys >> 8), static_cast<uint8_t>(ys),
                             static_cast<uint8_t>(ye >> 8), static_cast<uint8_t>(ye) };
    esp_lcd_panel_io_tx_param(g_io, 0x2A, ca, sizeof(ca));
    esp_lcd_panel_io_tx_param(g_io, 0x2B, ra, sizeof(ra));
}

// Blocking RAMWR: waits for the DMA completion callback so the caller can
// reuse the staging buffer as soon as this returns.
void push_colors(const uint16_t* px, size_t count) {
    esp_lcd_panel_io_tx_color(g_io, 0x2C, px, count * sizeof(uint16_t));
    xSemaphoreTake(g_tx_done, portMAX_DELAY);
}

}  // namespace

bool tft_init(const TftConfig& cfg) {
    if (cfg.width != kNativeH || cfg.height != kNativeW) {
        ESP_LOGE(TAG, "NV3007 expects a 428x142 canvas, got %dx%d", cfg.width, cfg.height);
        return false;
    }
    g_w = cfg.width;
    g_h = cfg.height;

    // Backlight: dark now, raised on after the first frame (ui.cpp).
    backlight_backend_init(cfg.backlight_gpio);

    g_tx_done = xSemaphoreCreateBinary();
    if (!g_tx_done) return false;

    spi_bus_config_t bus{};
    bus.mosi_io_num     = cfg.mosi_gpio;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = cfg.clk_gpio;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = sizeof(s_xpose);

    if (spi_bus_initialize(static_cast<spi_host_device_t>(cfg.spi_host), &bus, SPI_DMA_CH_AUTO) !=
        ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.dc_gpio_num         = cfg.dc_gpio;
    io_cfg.cs_gpio_num         = cfg.cs_gpio;
    io_cfg.pclk_hz             = cfg.freq_hz;
    io_cfg.lcd_cmd_bits        = 8;
    io_cfg.lcd_param_bits      = 8;
    io_cfg.spi_mode            = 0;
    io_cfg.trans_queue_depth   = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;

    if (esp_lcd_new_panel_io_spi(static_cast<spi_host_device_t>(cfg.spi_host), &io_cfg, &g_io) !=
        ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi failed");
        return false;
    }

    // Hardware reset pulse, then the DCS wake-up margin before any command.
    if (cfg.rst_gpio >= 0) {
        gpio_config_t rst{};
        rst.pin_bit_mask = 1ULL << cfg.rst_gpio;
        rst.mode         = GPIO_MODE_OUTPUT;
        gpio_config(&rst);
        gpio_set_level(static_cast<gpio_num_t>(cfg.rst_gpio), 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(cfg.rst_gpio), 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    for (size_t i = 0; i < sizeof(kInitSeq);) {
        const uint8_t cmd = kInitSeq[i];
        const uint8_t len = kInitSeq[i + 1];
        esp_lcd_panel_io_tx_param(g_io, cmd, &kInitSeq[i + 2], len);
        i += 2u + len;
    }
    esp_lcd_panel_io_tx_param(g_io, 0x11, nullptr, 0);  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(220));

    // Paint GRAM black before enabling the display, else the panel's random
    // power-on contents flash until the first UI frame is pushed. s_xpose is
    // still all-zero (.bss) here.
    for (int y = 0; y < kNativeH; y += kXposeRows) {
        const int rows = (kNativeH - y < kXposeRows) ? (kNativeH - y) : kXposeRows;
        set_window(0, kNativeW - 1, y, y + rows - 1);
        push_colors(s_xpose, static_cast<size_t>(rows) * kNativeW);
    }
    esp_lcd_panel_io_tx_param(g_io, 0x29, nullptr, 0);  // DISPON
    vTaskDelay(pdMS_TO_TICKS(200));

#ifdef CONFIG_PIXFROG_NV3007_ROT180
    ESP_LOGI(TAG, "NV3007 %dx%d ready (landscape, rot180)", g_w, g_h);
#else
    ESP_LOGI(TAG, "NV3007 %dx%d ready (landscape)", g_w, g_h);
#endif
    return true;
}

// Landscape rect in, portrait GRAM out. `data` is row-major landscape,
// (x2-x1) wide, already in panel byte order (canvas stores big-endian).
void tft_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t* data) {
    const int w_l = x2 - x1;
    if (w_l <= 0 || y2 <= y1) return;
    for (int cy = y1; cy < y2; cy += kXposeRows) {
        const int rows = (y2 - cy < kXposeRows) ? (y2 - cy) : kXposeRows;
#ifdef CONFIG_PIXFROG_NV3007_ROT180
        // landscape (x,y) → native (y, kNativeH-1-x)
        const int xs = cy;
        const int ys = kNativeH - x2;
        for (int yn = 0; yn < w_l; ++yn) {
            const int xl = x2 - 1 - yn;  // landscape column for native row ys+yn
            for (int xn = 0; xn < rows; ++xn)
                s_xpose[static_cast<long>(yn) * rows + xn] =
                    data[static_cast<long>(cy + xn - y1) * w_l + (xl - x1)];
        }
#else
        // landscape (x,y) → native (kNativeW-1-y, x)
        const int xs = kNativeW - (cy + rows);
        const int ys = x1;
        for (int yn = 0; yn < w_l; ++yn) {
            const int xl = x1 + yn;  // landscape column for native row ys+yn
            for (int xn = 0; xn < rows; ++xn) {
                const int yl = kNativeW - 1 - (xs + xn);  // landscape row
                s_xpose[static_cast<long>(yn) * rows + xn] =
                    data[static_cast<long>(yl - y1) * w_l + (xl - x1)];
            }
        }
#endif
        set_window(xs, xs + rows - 1, ys, ys + w_l - 1);
        push_colors(s_xpose, static_cast<size_t>(rows) * w_l);
    }
}

int tft_width() {
    return g_w;
}
int tft_height() {
    return g_h;
}

void* tft_fb_alloc(unsigned long bytes) {
    // Frame-buffer-sized → PSRAM (SRAM is reserved; see AGENT.md). The flush
    // path stages each band through s_xpose in internal RAM, so the panel DMA
    // never sources from PSRAM.
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace pixfrog::ui::detail
