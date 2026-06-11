// PARLIO TX backend for the 16-bit LED bus — ESP32-P4.
//
// Alternative to the LCD_CAM RGB backend (lcd_cam_output.cpp), selected with
// CONFIG_PIXFROG_LED_OUTPUT_PARLIO. Implements the same pixfrog::lcd::
// interface so main.cpp and the control console are backend-agnostic.
//
// Why PARLIO: the TX unit is a plain "N-bit bus + clock, fed by DMA" engine —
// no LCD line/frame timing registers, so none of the 12-bit h_res ceilings
// and no inter-line blanks. In loop transmission mode (EOF = DMA, not the
// 19-bit bit-count register) the frame length is bounded only by the DMA
// link list, and the hardware repeats the mounted frame back-to-back with no
// gap — the encoded reset tail separates repeats, so LEDs simply see the
// last frame re-sent continuously, like a video signal.
//
// Double buffering: parlio_tx_unit_transmit() called while a loop
// transmission is running does NOT queue a new transaction — the driver
// mounts the new payload on its second DMA link list and concatenates it to
// the tail of the running one, so the swap happens at the next frame
// boundary, gapless. There is no completion event in loop mode (no DMA EOF
// is marked), so the buffer being replaced is known free only one frame
// duration after the swap was submitted — render_frame() enforces that wait
// before encoding into it.
//
// Clock: PLL_F160M / 10 = 16 MHz exactly (led_protocols::kPclkHz).

#include "sdkconfig.h"
#if CONFIG_PIXFROG_LED_OUTPUT_PARLIO

#include "lcd_cam_output.h"

#include <cstring>

#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"
#include "output_common.h"

namespace pixfrog::lcd {

namespace {

using common::desc_for_channel;
using common::required_frame_samples;
using common::round_up;

constexpr const char* TAG = "PARLIO_OUT";

// parlio_tx_unit_transmit() requires the payload address aligned to the
// external-memory DMA alignment and the bit length aligned to the same
// value; 64 samples = 128 B = 1024 bits satisfies every P4 cache-line /
// burst combination, and conveniently extends the frame with LOW samples
// (reset-tail territory).
constexpr size_t kFrameQuantumSamples = 64;
constexpr size_t kFbAlignBytes        = 128;

// Same scope-friendly calibration frame length as the LCD_CAM backend.
constexpr size_t kCalFrameSamples = 262144;

InitConfig g_cfg{};
parlio_tx_unit_handle_t g_unit = nullptr;
uint16_t* g_fb[2]              = { nullptr, nullptr };
size_t g_fb_samples            = 0;         // capacity of each FB, quantum-rounded
uint8_t g_back_idx             = 0;         // FB the next encode writes into
size_t g_written[2]            = { 0, 0 };  // samples encoded into each FB since creation
bool g_loop_running            = false;
int64_t g_last_submit_us       = 0;  // when the running loop frame was (re)submitted
volatile uint32_t g_submits    = 0;

volatile int8_t g_cal_mode = -1;

int64_t frame_duration_us() {
    return static_cast<int64_t>(g_fb_samples) * 1'000'000LL / g_cfg.pclk_hz;
}

void destroy_unit() {
    if (g_unit) {
        if (g_loop_running) parlio_tx_unit_disable(g_unit);
        parlio_del_tx_unit(g_unit);
        g_unit = nullptr;
    }
    for (auto& fb : g_fb) {
        if (fb) heap_caps_free(fb);
        fb = nullptr;
    }
    g_fb_samples     = 0;
    g_loop_running   = false;
    g_last_submit_us = 0;
}

bool create_unit(size_t samples) {
    const size_t bytes = samples * sizeof(uint16_t);
    for (auto& fb : g_fb) {
        fb = static_cast<uint16_t*>(
            heap_caps_aligned_calloc(kFbAlignBytes, 1, bytes, MALLOC_CAP_SPIRAM));
        if (!fb) {
            ESP_LOGE(TAG, "FB alloc failed (%zu bytes in PSRAM)", bytes);
            destroy_unit();
            return false;
        }
    }

    parlio_tx_unit_config_t cfg{};
    cfg.clk_src            = PARLIO_CLK_SRC_DEFAULT;  // PLL_F160M → /10 = 16 MHz exact
    cfg.clk_in_gpio_num    = GPIO_NUM_NC;
    cfg.output_clk_freq_hz = g_cfg.pclk_hz;
    cfg.data_width         = 16;
    for (int i = 0; i < 16; ++i) {
        cfg.data_gpio_nums[i] = static_cast<gpio_num_t>(g_cfg.bus_gpio_16[i]);
    }
    cfg.clk_out_gpio_num  = GPIO_NUM_NC;  // per-channel clocks are data bits, PCLK stays internal
    cfg.valid_gpio_num    = GPIO_NUM_NC;
    cfg.trans_queue_depth = 4;
    cfg.max_transfer_size = bytes;
    cfg.dma_burst_size    = 64;
    cfg.sample_edge       = PARLIO_SAMPLE_EDGE_POS;

    esp_err_t err = parlio_new_tx_unit(&cfg, &g_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parlio_new_tx_unit: %s", esp_err_to_name(err));
        destroy_unit();
        return false;
    }
    if ((err = parlio_tx_unit_enable(g_unit)) != ESP_OK) {
        ESP_LOGE(TAG, "parlio_tx_unit_enable: %s", esp_err_to_name(err));
        destroy_unit();
        return false;
    }

    g_fb_samples   = samples;
    g_back_idx     = 0;
    g_written[0]   = 0;
    g_written[1]   = 0;
    g_loop_running = false;

    ESP_LOGI(TAG,
             "tx unit up: fb_a=%p fb_b=%p, %zu bytes each, %zu samples @ %lu Hz "
             "(%lld µs/frame, loop)",
             static_cast<void*>(g_fb[0]), static_cast<void*>(g_fb[1]), bytes, samples,
             static_cast<unsigned long>(g_cfg.pclk_hz), frame_duration_us());
    return true;
}

bool ensure_frame_capacity(size_t needed_samples) {
    if (needed_samples > g_cfg.max_samples_per_frame) {
        ESP_LOGE(TAG, "frame needs %zu samples > cap %lu — config should not have validated",
                 needed_samples, static_cast<unsigned long>(g_cfg.max_samples_per_frame));
        return false;
    }
    const size_t samples = round_up(needed_samples, kFrameQuantumSamples);
    if (g_unit && samples == g_fb_samples) return true;
    destroy_unit();
    return create_unit(samples);
}

// The buffer that is about to be encoded may still be scanned out by the
// running loop until one frame duration after the swap that retired it was
// submitted. Block (cheaply) until that point.
void wait_back_buffer_free() {
    if (!g_loop_running) return;
    const int64_t free_at = g_last_submit_us + frame_duration_us();
    int64_t now           = esp_timer_get_time();
    while (now < free_at) {
        const int64_t remaining_us = free_at - now;
        vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000 + 1));
        now = esp_timer_get_time();
    }
}

// Submit `fb` as the (new) looping frame. First call starts the loop; later
// calls swap the mounted buffer at the next frame boundary, gapless. The
// driver write-backs the payload cache itself.
bool submit_loop_frame(uint16_t* fb) {
    parlio_transmit_config_t tx{};
    tx.idle_value              = 0;
    tx.flags.loop_transmission = 1;
    const size_t payload_bits  = g_fb_samples * 16;
    const esp_err_t err        = parlio_tx_unit_transmit(g_unit, fb, payload_bits, &tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transmit: %s", esp_err_to_name(err));
        return false;
    }
    g_last_submit_us = esp_timer_get_time();
    g_loop_running   = true;
    g_submits        = g_submits + 1;
    return true;
}

}  // namespace

bool init(const InitConfig& cfg) {
    g_cfg = cfg;

    if (cfg.max_samples_per_frame == 0 || cfg.bus_gpio_16 == nullptr) {
        ESP_LOGE(TAG, "invalid InitConfig");
        return false;
    }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        ESP_LOGE(TAG, "no PSRAM detected; ESP32-P4 octal PSRAM is mandatory");
        return false;
    }

    // All channels Off at boot is legal: the unit is created lazily by the
    // first frame that has something to emit.
    const size_t needed = required_frame_samples();
    if (needed > 0 && !ensure_frame_capacity(needed)) return false;

    ESP_LOGI(TAG, "init OK (%s)", needed > 0 ? "tx unit created" : "all channels off, unit lazy");
    return true;
}

bool render_frame(uint32_t timeout_ms) {
    (void)timeout_ms;  // pacing is wait_back_buffer_free(), bounded by one frame
    const size_t needed = required_frame_samples();
    if (needed == 0) {
        // Every channel Off: stop the loop so the bus goes quiet.
        if (g_unit) destroy_unit();
        return true;
    }
    if (!ensure_frame_capacity(needed)) return false;

    wait_back_buffer_free();
    uint16_t* samples = g_fb[g_back_idx];

    led::ChannelDesc descs[config::kNumChannels];
    const uint8_t* pixels[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        descs[ch]  = desc_for_channel(ch);
        pixels[ch] = dmx::pixel_front_buffer(ch);
    }

    const size_t written = led::encode_frame(descs, pixels, config::kNumChannels, samples,
                                             g_fb_samples);
    if (written == 0) {
        ESP_LOGE(TAG, "encode_frame wrote nothing (needed=%zu capacity=%zu)", needed, g_fb_samples);
        return false;
    }

    // The loop always scans the full quantum-rounded buffer; everything past
    // `written` must be LOW (reset-tail). calloc zeroed it at creation, but a
    // shrinking frame within the same capacity leaves stale samples behind.
    if (g_written[g_back_idx] > written) {
        std::memset(samples + written, 0, (g_written[g_back_idx] - written) * sizeof(uint16_t));
    }
    g_written[g_back_idx] = written;

    if (!submit_loop_frame(samples)) return false;

    g_back_idx ^= 1;
    dmx::note_frame_emitted();
    return true;
}

bool emit_calibration_pattern(uint8_t pattern_id) {
    if (pattern_id == 3) {
        common::gpio_bitbang_probe_tick(g_cfg.bus_gpio_16);
        return true;
    }

    if (!ensure_frame_capacity(kCalFrameSamples)) return false;

    wait_back_buffer_free();
    uint16_t* samples = g_fb[g_back_idx];
    common::fill_calibration_pattern(samples, g_fb_samples, pattern_id, g_cfg.pclk_hz);
    g_written[g_back_idx] = g_fb_samples;

    if (!submit_loop_frame(samples)) return false;
    g_back_idx ^= 1;
    return true;
}

void dump_stats() {
    ESP_LOGI(TAG, "stats: submits=%lu, frame=%lld µs, loop=%d",
             static_cast<unsigned long>(g_submits), frame_duration_us(), g_loop_running ? 1 : 0);
}

DebugCounters get_debug_counters() {
    DebugCounters c{};
    // Loop transmissions generate no DMA EOF and no LCD vsync — submit count
    // is the only meaningful per-frame counter on this backend.
    c.trans_done = g_submits;
    c.vsync      = g_submits;
    c.msync_err  = 0;
    return c;
}

void wait_idle() {
    // A loop transmission never idles by design; the closest equivalent is
    // "the most recently submitted frame is now the one on the wire".
    wait_back_buffer_free();
}

size_t fb_bytes() {
    return g_fb_samples * sizeof(uint16_t);
}

void set_calibration_mode(int8_t pattern_id) {
    g_cal_mode = pattern_id;
}
int8_t get_calibration_mode() {
    return g_cal_mode;
}

}  // namespace pixfrog::lcd

#endif  // CONFIG_PIXFROG_LED_OUTPUT_PARLIO
