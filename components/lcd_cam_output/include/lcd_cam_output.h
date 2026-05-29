// lcd_cam_output — drives the ESP32-P4 LCD_CAM peripheral as a free-running
// 16-bit parallel bus with GDMA, emitting LED data for 8 channels at once.
//
// Architecture (cf. docs/ARCHITECTURE.md §4):
//   Two frame buffers live in PSRAM, owned by esp_lcd_rgb_panel.
//   render_task encodes the full next frame into fb_back,
//   calls esp_cache_msync(DIR_C2M) + esp_lcd_panel_draw_bitmap(),
//   and waits on done_sem (released by on_color_trans_done ISR).
//
//   No chunked refill. No real-time sub-frame deadline on the encoder.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::lcd {

struct InitConfig {
    const int* bus_gpio_16;          // bit k of the bus → GPIO bus_gpio_16[k]
    uint32_t   pclk_hz;              // PCLK frequency; must match led_protocols::kPclkHz
    uint32_t   max_samples_per_frame; // sizes the PSRAM frame buffers; cf. led::encoded_size_samples
};

// Initialize the LCD_CAM peripheral and allocate the two PSRAM frame buffers
// via esp_lcd_new_rgb_panel(flags.fb_in_psram=true, num_fbs=2).
// Returns false on any IDF API failure (most often: PSRAM too small).
bool init(const InitConfig& cfg);

// Encode the next frame from dmx_manager's front pixel buffers into the back
// frame buffer, sync cache, and trigger emission. Blocks waiting for the
// previous emission to complete (up to `timeout_ms`); on timeout, increments
// `dma_underruns` and returns false but still leaves the next frame queued.
bool render_frame(uint32_t timeout_ms = 50);

// Block until any in-flight emission completes. Useful at shutdown.
void wait_idle();

// Returns the actual frame buffer size in bytes that was allocated per FB.
// Useful for boot-time diagnostics.
size_t fb_bytes();

}  // namespace pixfrog::lcd
