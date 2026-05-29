// lcd_cam_output — drives the ESP32-P4 LCD_CAM peripheral as a free-running
// 16-bit parallel bus with GDMA, emitting LED data for 8 channels at once.
//
// Frame lifecycle (cf. docs/ARCHITECTURE.md §4):
//   render_task → fill pixel_buf[ch][back]
//                → call lcd_cam_render_frame()
//                   → encode every channel into chunked DMA buffers
//                   → kick LCD_CAM peripheral
//                   → ISR EOF refills chunks until done
//                → frame complete

#pragma once

#include <stdint.h>

namespace pixfrog::lcd {

struct InitConfig {
    const int* bus_gpio_16;   // 16-element array; lcd_cam bus bit k → GPIO bus_gpio_16[k]
    uint32_t   pclk_hz;       // configured PCLK; must match led_protocols::kPclkHz
    int        dma_chunk_pixels;  // pixels worth of samples per DMA chunk; 16 default
    int        dma_chunk_count;   // 3 = triple buffer
};

// Initialize the LCD_CAM driver. Returns false on any IDF API failure.
bool init(const InitConfig& cfg);

// Kick off rendering of one frame using the front pixel buffers from
// dmx_manager. Blocks until DMA has been queued (NOT until completion).
// Returns false if a previous frame is still emitting.
bool render_frame();

// Block until the current DMA chain has fully drained.
// Useful at shutdown or for tests.
void wait_idle();

}  // namespace pixfrog::lcd
