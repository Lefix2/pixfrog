// led_output — drives the ESP32-P4 16-bit parallel LED bus with GDMA,
// emitting LED data for 8 channels at once. Two interchangeable backends
// implement this interface, picked by Kconfig: PARLIO TX in loop mode
// (default) or the legacy LCD_CAM RGB panel.
//
// Architecture (cf. docs/ARCHITECTURE.md §4):
//   The frame buffers live in PSRAM (three on PARLIO, two on LCD_CAM). PARLIO
//   allocates them once at `max_samples_per_frame` and varies only the emitted
//   length per transmit; LCD_CAM carries the length in the panel's timing
//   registers, so it sizes them to the current config and recreates the panel
//   on a config commit that changes that length.
//   render_task encodes the full next frame into fb_back in one pass
//   (led::encode_frame) while the previous frame is still emitting from the
//   other FB, calls esp_cache_msync(DIR_C2M) on the written region, waits on
//   done_sem (released by on_color_trans_done ISR) and kicks
//   esp_lcd_panel_draw_bitmap() — encode and emission overlap.
//
//   No chunked refill. No real-time sub-frame deadline on the encoder.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::output {

struct InitConfig {
    const int* bus_gpio_16;          // bit k of the bus → GPIO bus_gpio_16[k]
    uint32_t pclk_hz;                // PCLK frequency; must match led_protocols::kPclkHz
    uint32_t max_samples_per_frame;  // hard upper cap on the frame length; PARLIO sizes
                                     // its frame buffers to exactly this, LCD_CAM only
                                     // validates against it
};

// Initialize the active output backend, allocating everything the render path
// will ever touch: on PARLIO the TX unit and three frame buffers at the cap,
// on LCD_CAM a panel sized to the current config (or none, if every channel is
// Off at boot — LCD_CAM creates it lazily).
// Returns false on any IDF API failure (most often: PSRAM too small).
bool init(const InitConfig& cfg);

// Encode the next frame from dmx_manager's front pixel buffers into the back
// frame buffer (single pass, while the previous emission drains from the
// other FB), sync cache on the written region, and trigger emission. Waits
// for the previous emission only between encode and kick (up to
// `timeout_ms`); on timeout, increments `dma_underruns` and returns false.
// When every channel is Off, returns true without touching the hardware.
bool render_frame(uint32_t timeout_ms = 50);

// Block until any in-flight emission completes. Useful at shutdown.
void wait_idle();

// Returns the actual frame buffer size in bytes that was allocated per FB.
// Useful for boot-time diagnostics.
size_t fb_bytes();

// Log a one-line summary of the backend's telemetry counters:
//   on_color_trans_done / on_vsync fire counts,
//   expected DMA duration vs measured inter-kick interval.
// Call periodically from render_task or on a 1-Hz timer to monitor
// PSRAM→GDMA sustained throughput in production.
void dump_stats();

// Raw telemetry counters for the control console: ISR fire counts for the
// two completion callbacks, and esp_cache_msync failures (a non-zero count
// means GDMA may be emitting stale FB contents).
struct DebugCounters {
    uint32_t trans_done;
    uint32_t vsync;
    uint32_t msync_err;
    // Per-phase timing of the most recent render_frame (µs). On the PARLIO
    // backend these expose whether encode/flush overlaps the wire emission:
    // a near-zero wait_us means the buffer was already drained (triple-buffer
    // pipeline keeping up). Zero on the LCD_CAM backend.
    uint32_t wait_us;
    uint32_t encode_us;
    uint32_t submit_us;
};
DebugCounters get_debug_counters();

// Emit a calibration pattern instead of pixel data. Used at bring-up to
// validate LCD_CAM GPIO routing, PCLK timing, and PSRAM→GDMA throughput
// with an oscilloscope. The frame is resized to a fixed 16.4 ms scope-
// friendly length, independent of the LED config. Pattern IDs:
//   0  — 1 kHz square wave on every bus bit. Each GPIO should toggle at
//        exactly 1 kHz; use to verify pin assignments and check for shorts.
//   1  — walking-1 across bits 0..15. Only one GPIO is HIGH at any time;
//        use to verify bit ordering matches kLedBusGpio.
//   2  — alternating 0xAAAA / 0x5555 per sample. Verifies that PCLK
//        ticks match the configured rate and that no bits are stuck.
// Returns false if a previous emission has not yet completed.
bool emit_calibration_pattern(uint8_t pattern_id);

// Persistent calibration mode. Set to -1 to resume normal LED
// rendering, or to 0..2 to make render_task continuously emit the chosen
// calibration pattern instead. The flag is sampled at the start of every
// render iteration; switching modes takes effect on the next frame.
void set_calibration_mode(int8_t pattern_id);
int8_t get_calibration_mode();

}  // namespace pixfrog::output
