// LCD_CAM 16-bit free-running output driver — ESP32-P4.
//
// Uses esp_lcd_new_rgb_panel in `refresh_on_demand` mode with two frame
// buffers in PSRAM. Each call to render_frame() encodes the next frame
// into the back FB and triggers GDMA emission via esp_lcd_panel_draw_bitmap.
//
// Status: implemented surface — register-level details validated against
// IDF v5.3 RGB panel API. The 1-line frame buffer model (h_res = total
// samples, v_res = 1, no hsync/vsync porches) needs validation on real
// hardware; some IDF builds reject hsync_pulse_width = 0 and may need 1.

#include "lcd_cam_output.h"

#include <cstring>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"

namespace pixfrog::lcd {

namespace {

constexpr const char* TAG = "LCD_CAM";

InitConfig             g_cfg{};
esp_lcd_panel_handle_t g_panel = nullptr;
void*                  g_fb_a  = nullptr;
void*                  g_fb_b  = nullptr;
size_t                 g_fb_bytes = 0;
size_t                 g_fb_h_res = 0;     // samples per "line" (= total samples per frame)
uint8_t                g_back_idx = 0;     // which fb is next to write into (0 → write fb_a)

SemaphoreHandle_t      g_done_sem = nullptr;

bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t /*panel*/,
                             const esp_lcd_rgb_panel_event_data_t* /*edata*/,
                             void* /*user_ctx*/) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_done_sem, &hp);
    return hp == pdTRUE;
}

// Build a led::ChannelDesc from the current ChannelConfig + bus bit assignment.
led::ChannelDesc desc_for_channel(size_t ch) {
    const auto& cc = config::get_channel(ch);
    led::ChannelDesc d{};
    d.protocol         = cc.protocol;
    d.color_order      = cc.color_order;
    d.pixel_count      = cc.pixel_count;
    d.brightness       = cc.brightness;
    d.grouping         = cc.grouping;
    d.invert_direction = cc.invert_direction;
    d.bus_bit_data     = static_cast<uint8_t>(ch * 2);
    d.bus_bit_clock    = static_cast<uint8_t>(ch * 2 + 1);
    d.clock_hz         = cc.clock_hz;
    return d;
}

}  // namespace

bool init(const InitConfig& cfg) {
    g_cfg = cfg;

    if (cfg.max_samples_per_frame == 0 || cfg.bus_gpio_16 == nullptr) {
        ESP_LOGE(TAG, "invalid InitConfig");
        return false;
    }

    g_fb_h_res = cfg.max_samples_per_frame;
    g_fb_bytes = g_fb_h_res * sizeof(uint16_t);

    esp_lcd_rgb_panel_config_t panel_config{};
    panel_config.data_width             = 16;
    panel_config.num_fbs                = 2;
    panel_config.bounce_buffer_size_px  = 0;
    panel_config.clk_src                = LCD_CLK_SRC_DEFAULT;
    panel_config.disp_gpio_num          = -1;
    panel_config.pclk_gpio_num          = -1;
    panel_config.hsync_gpio_num         = -1;
    panel_config.vsync_gpio_num         = -1;
    panel_config.de_gpio_num            = -1;
    for (int i = 0; i < 16; ++i) {
        panel_config.data_gpio_nums[i] = cfg.bus_gpio_16[i];
    }
    panel_config.timings.pclk_hz            = cfg.pclk_hz;
    panel_config.timings.h_res              = g_fb_h_res;
    panel_config.timings.v_res              = 1;
    panel_config.timings.hsync_pulse_width  = 1;   // IDF rejects 0; the pin is not routed anyway
    panel_config.timings.hsync_back_porch   = 0;
    panel_config.timings.hsync_front_porch  = 0;
    panel_config.timings.vsync_pulse_width  = 1;
    panel_config.timings.vsync_back_porch   = 0;
    panel_config.timings.vsync_front_porch  = 0;
    panel_config.timings.flags.pclk_active_neg = 0;
    panel_config.flags.fb_in_psram          = 1;
    panel_config.flags.refresh_on_demand    = 1;
    panel_config.flags.bb_invalidate_cache  = 0;   // we manage cache ourselves

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &g_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_rgb_panel_event_callbacks_t cbs{};
    cbs.on_color_trans_done = on_trans_done;
    esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);

    if ((err = esp_lcd_panel_reset(g_panel)) != ESP_OK) {
        ESP_LOGE(TAG, "panel_reset: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_lcd_panel_init(g_panel)) != ESP_OK) {
        ESP_LOGE(TAG, "panel_init: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, &g_fb_a, &g_fb_b);
    if (err != ESP_OK || !g_fb_a || !g_fb_b) {
        ESP_LOGE(TAG, "get_frame_buffer: %s", esp_err_to_name(err));
        return false;
    }
    std::memset(g_fb_a, 0, g_fb_bytes);
    std::memset(g_fb_b, 0, g_fb_bytes);

    g_done_sem = xSemaphoreCreateBinary();
    if (!g_done_sem) return false;
    xSemaphoreGive(g_done_sem);   // start idle so the first render_frame doesn't block

    ESP_LOGI(TAG, "init OK: fb_a=%p fb_b=%p, %zu bytes each, h_res=%zu @ %lu Hz",
             g_fb_a, g_fb_b, g_fb_bytes, g_fb_h_res, static_cast<unsigned long>(cfg.pclk_hz));
    return true;
}

bool render_frame(uint32_t timeout_ms) {
    if (!g_panel) return false;

    // Wait for the previous emission to drain so the FB we are about to write
    // is no longer being read by GDMA.
    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        dmx::note_dma_underrun();
        return false;
    }

    void* back = (g_back_idx == 0) ? g_fb_a : g_fb_b;
    uint16_t* samples = static_cast<uint16_t*>(back);

    // Zero the buffer so unused bus bits stay LOW (TRESET-compliant tail) and
    // so encoder OR-semantics start clean.
    std::memset(samples, 0, g_fb_bytes);

    // Encode each channel. Channels share the buffer via OR on their bus bits.
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const led::ChannelDesc d = desc_for_channel(ch);
        const uint8_t* px = dmx::pixel_front_buffer(ch);
        if (!px) continue;
        led::encode_channel(d, px, samples, g_fb_h_res);
    }

    // Make the CPU writes visible to GDMA. PSRAM cache is write-back; without
    // this, GDMA could fetch stale data and emit garbage.
    esp_cache_msync(samples, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // Hand the back FB to the panel as the next frame to emit. With num_fbs=2
    // and `refresh_on_demand`, IDF detects that the passed buffer matches one
    // of its internal FBs and simply switches the active pointer (no copy).
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        g_panel, 0, 0, static_cast<int>(g_fb_h_res), 1, samples);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        // We still gave the sem back logically; restore it so the next call doesn't deadlock.
        xSemaphoreGive(g_done_sem);
        return false;
    }

    g_back_idx ^= 1;
    dmx::note_frame_emitted();
    return true;
}

void wait_idle() {
    if (g_done_sem && xSemaphoreTake(g_done_sem, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(g_done_sem);
    }
}

size_t fb_bytes() { return g_fb_bytes; }

}  // namespace pixfrog::lcd
