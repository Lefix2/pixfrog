// LCD_CAM 16-bit free-running output driver — ESP32-P4.
//
// STATUS: SKELETON. The function bodies below describe the intended flow but
// the actual register-level configuration is left to a follow-up commit that
// will be validated on real hardware. Reasons:
//
//   1. The ESP-IDF esp_lcd RGB panel API (`esp_lcd_new_rgb_panel`) is the
//      cleanest way to drive the 16-bit bus with PCLK, but its docs differ
//      between IDF v5.3 and v5.4 on ESP32-P4. Validation requires the chip.
//   2. We need to disable HSYNC/VSYNC framing (we don't want any picture
//      structure, just a continuous DMA-driven sample stream).
//   3. EOF callbacks must be wired to a refill task that re-encodes the
//      "next chunk" of pixel data into the just-emitted DMA buffer.
//
// The skeleton compiles cleanly (uses only public headers) and exposes the
// surface that render_task can call.

#include "lcd_cam_output.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"

namespace pixfrog::lcd {

namespace {

constexpr const char* TAG = "LCD_CAM";

InitConfig         g_cfg{};
SemaphoreHandle_t  g_busy_sem = nullptr;     // taken while a frame is in flight
SemaphoreHandle_t  g_refill_sem = nullptr;   // given by EOF ISR

// TODO: hold the esp_lcd_panel_handle_t / esp_lcd_panel_io_handle_t here
// once the IDF API selection is finalized.

bool encode_all_channels_into_buffer(uint16_t* /*dma_buf*/, size_t /*samples_capacity*/) {
    // For each of the 8 channels:
    //   load current ChannelConfig from cache
    //   pull pixel_front_buffer(ch) from dmx_manager
    //   build the led::ChannelDesc with bus_bit_data = ch*2, bus_bit_clock = ch*2+1
    //   call led::encode_channel(desc, pixels, dma_buf, samples_capacity)
    // Note: encode_channel ORs its bits into dma_buf, so the buffer MUST be
    // zeroed before the first channel encodes.
    //
    // The actual chunked variant will encode samples lazily — only the next
    // `g_cfg.dma_chunk_pixels` worth — so we do not have to hold a whole frame
    // of samples in RAM. That refactor is required before this is functional.
    return true;
}

void IRAM_ATTR on_eof_isr_stub() {
    // BaseType_t hp = pdFALSE;
    // xSemaphoreGiveFromISR(g_refill_sem, &hp);
    // if (hp) portYIELD_FROM_ISR();
}

}  // namespace

bool init(const InitConfig& cfg) {
    g_cfg = cfg;
    g_busy_sem   = xSemaphoreCreateBinary();
    g_refill_sem = xSemaphoreCreateBinary();
    if (!g_busy_sem || !g_refill_sem) return false;
    xSemaphoreGive(g_busy_sem);  // start idle

    // TODO: actually set up the LCD_CAM peripheral via esp_lcd_new_rgb_panel
    // with the parameters in `cfg`. The expected approach:
    //
    //   esp_lcd_rgb_panel_config_t panel_cfg{};
    //   panel_cfg.clk_src                  = LCD_CLK_SRC_DEFAULT;
    //   panel_cfg.data_width               = 16;
    //   panel_cfg.psram_trans_align        = 0;       // SRAM DMA only
    //   panel_cfg.timings.pclk_hz          = cfg.pclk_hz;
    //   panel_cfg.timings.h_res            = …;       // packed-sample width
    //   panel_cfg.timings.v_res            = 1;       // single line
    //   panel_cfg.timings.hsync_pulse_width= 0;
    //   panel_cfg.timings.hsync_back_porch = 0;
    //   panel_cfg.timings.hsync_front_porch= 0;
    //   panel_cfg.timings.vsync_*          = 0;
    //   panel_cfg.flags.no_hsync_pulse     = true;
    //   panel_cfg.flags.no_vsync_pulse     = true;
    //   for (int i = 0; i < 16; ++i) panel_cfg.data_gpio_nums[i] = cfg.bus_gpio_16[i];
    //   panel_cfg.flags.fb_in_psram        = false;
    //   panel_cfg.bounce_buffer_size_px    = cfg.dma_chunk_pixels;
    //   ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_cfg, &g_panel), TAG, "rgb_panel");
    //   esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);
    //   esp_lcd_panel_init(g_panel);

    ESP_LOGW(TAG, "init skeleton — actual LCD_CAM bring-up TODO");
    return true;
}

bool render_frame() {
    if (xSemaphoreTake(g_busy_sem, 0) != pdTRUE) return false;
    // TODO:
    //   - obtain a free DMA chunk
    //   - zero it
    //   - encode_all_channels_into_buffer(chunk, capacity)
    //   - enqueue chunk to the LCD_CAM panel
    //   - return — EOF ISR continues with next chunks
    dmx::note_frame_emitted();
    xSemaphoreGive(g_busy_sem);  // skeleton: complete immediately
    return true;
}

void wait_idle() {
    if (xSemaphoreTake(g_busy_sem, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(g_busy_sem);
    }
}

}  // namespace pixfrog::lcd
