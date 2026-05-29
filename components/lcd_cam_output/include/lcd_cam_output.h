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

// Log a one-line summary of LCD_CAM telemetry counters:
//   on_color_trans_done / on_vsync fire counts (item 4),
//   expected DMA duration vs measured inter-kick interval (item 5).
// Call periodically from render_task or on a 1-Hz timer to monitor
// PSRAM→GDMA sustained throughput in production.
void dump_stats();

// Emit a calibration pattern instead of pixel data. Used at bring-up to
// validate LCD_CAM GPIO routing, PCLK timing, and PSRAM→GDMA throughput
// with an oscilloscope. Pattern IDs:
//   0  — 1 kHz square wave on every bus bit. Each GPIO should toggle at
//        exactly 1 kHz; use to verify pin assignments and check for shorts.
//   1  — walking-1 across bits 0..15. Only one GPIO is HIGH at any time;
//        use to verify bit ordering matches kLedBusGpio.
//   2  — alternating 0xAAAA / 0x5555 per sample. Verifies that PCLK
//        ticks match the configured rate and that no bits are stuck.
// Returns false if a previous emission has not yet completed.
bool emit_calibration_pattern(uint8_t pattern_id);

}  // namespace pixfrog::lcd
