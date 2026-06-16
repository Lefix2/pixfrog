// PARLIO TX backend for the 16-bit LED bus — ESP32-P4.
//
// Alternative to the LCD_CAM RGB backend (led_output.cpp), selected with
// CONFIG_PIXFROG_LED_OUTPUT_PARLIO. Implements the same pixfrog::output::
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

#include "led_output.h"

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

namespace pixfrog::output {

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

// Triple buffering. Loop mode keeps exactly two buffers in the DMA chain (the
// one scanning + the one concatenated as "next"); a third lets render_frame
// encode the upcoming frame into an already-drained buffer WHILE those two are
// busy, so the PSRAM encode+cache-flush (≈ one frame's worth of bus time)
// overlaps the wire emission instead of serializing after it. With only two
// buffers the buffer we must reuse is exactly the one freed one frame ago — no
// slack — forcing a full-frame wait before every encode and capping the rate
// at 1/(frame + encode). Three buffers give a full frame of drain slack.
constexpr size_t kNumFb = 3;

InitConfig g_cfg{};
parlio_tx_unit_handle_t g_unit = nullptr;
uint16_t* g_fb[kNumFb]         = {};
size_t g_fb_samples            = 0;   // capacity of each FB, quantum-rounded
uint8_t g_back_idx             = 0;   // FB the next encode writes into
size_t g_written[kNumFb]       = {};  // samples encoded into each FB since creation
// Wall-clock µs after which each FB is safe to encode into again. A transmit
// retires the *previously* submitted buffer: the hardware keeps scanning it
// until the current pass ends, at most one frame later.
int64_t g_free_at[kNumFb]   = {};
int8_t g_prev_submit_idx    = -1;  // buffer handed to the previous submit, or -1
volatile uint32_t g_submits = 0;

// Per-phase timing of the most recent render_frame, for the control console.
volatile uint32_t g_wait_us   = 0;
volatile uint32_t g_encode_us = 0;
volatile uint32_t g_submit_us = 0;

volatile int8_t g_cal_mode = -1;

int64_t frame_duration_us() {
    return static_cast<int64_t>(g_fb_samples) * 1'000'000LL / g_cfg.pclk_hz;
}

void destroy_unit() {
    if (g_unit) {
        if (g_prev_submit_idx >= 0) parlio_tx_unit_disable(g_unit);
        parlio_del_tx_unit(g_unit);
        g_unit = nullptr;
    }
    for (auto& fb : g_fb) {
        if (fb) heap_caps_free(fb);
        fb = nullptr;
    }
    for (auto& f : g_free_at)
        f = 0;
    g_fb_samples      = 0;
    g_prev_submit_idx = -1;
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

    g_fb_samples      = samples;
    g_back_idx        = 0;
    g_prev_submit_idx = -1;
    for (size_t i = 0; i < kNumFb; ++i) {
        g_written[i] = 0;
        g_free_at[i] = 0;
    }

    ESP_LOGI(TAG,
             "tx unit up: %zu buffers, %zu bytes each, %zu samples @ %lu Hz "
             "(%lld µs/frame, loop)",
             kNumFb, bytes, samples, static_cast<unsigned long>(g_cfg.pclk_hz),
             frame_duration_us());
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

// Buffer `idx` may still be scanned out by the running loop until the
// timestamp recorded when the transmit that retired it ran. Block (cheaply)
// until then — with triple buffering this is normally already in the past
// (a full frame of drain slack), so render_frame's encode overlaps emission.
void wait_back_buffer_free(uint8_t idx) {
    int64_t now = esp_timer_get_time();
    while (now < g_free_at[idx]) {
        const int64_t remaining_us = g_free_at[idx] - now;
        vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000 + 1));
        now = esp_timer_get_time();
    }
}

// Submit g_fb[idx] as the (new) looping frame. First call starts the loop;
// later calls swap the mounted buffer at the next frame boundary, gapless. The
// driver write-backs the payload cache itself.
bool submit_loop_frame(uint8_t idx) {
    parlio_transmit_config_t tx{};
    tx.idle_value              = 0;
    tx.flags.loop_transmission = 1;
    const size_t payload_bits  = g_fb_samples * 16;
    const esp_err_t err        = parlio_tx_unit_transmit(g_unit, g_fb[idx], payload_bits, &tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transmit: %s", esp_err_to_name(err));
        return false;
    }
    // This transmit concatenates idx after the previously submitted buffer,
    // which the hardware keeps scanning until its current pass ends — at most
    // one frame from now. Mark it safe to reuse only after that.
    const int64_t now = esp_timer_get_time();
    if (g_prev_submit_idx >= 0) {
        g_free_at[g_prev_submit_idx] = now + frame_duration_us();
    }
    g_prev_submit_idx = static_cast<int8_t>(idx);
    g_submits         = g_submits + 1;
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

    const uint8_t idx = g_back_idx;
    const int64_t t0  = esp_timer_get_time();
    wait_back_buffer_free(idx);
    uint16_t* samples = g_fb[idx];

    const int64_t t1 = esp_timer_get_time();
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
    if (g_written[idx] > written) {
        std::memset(samples + written, 0, (g_written[idx] - written) * sizeof(uint16_t));
    }
    g_written[idx] = written;

    const int64_t t2 = esp_timer_get_time();
    if (!submit_loop_frame(idx)) return false;
    const int64_t t3 = esp_timer_get_time();

    g_wait_us   = static_cast<uint32_t>(t1 - t0);
    g_encode_us = static_cast<uint32_t>(t2 - t1);
    g_submit_us = static_cast<uint32_t>(t3 - t2);

    g_back_idx = static_cast<uint8_t>((g_back_idx + 1) % kNumFb);
    dmx::note_frame_emitted();
    return true;
}

bool emit_calibration_pattern(uint8_t pattern_id) {
    if (pattern_id == 3) {
        common::gpio_bitbang_probe_tick(g_cfg.bus_gpio_16);
        return true;
    }

    if (!ensure_frame_capacity(kCalFrameSamples)) return false;

    const uint8_t idx = g_back_idx;
    wait_back_buffer_free(idx);
    uint16_t* samples = g_fb[idx];
    common::fill_calibration_pattern(samples, g_fb_samples, pattern_id, g_cfg.pclk_hz);
    g_written[idx] = g_fb_samples;

    if (!submit_loop_frame(idx)) return false;
    g_back_idx = static_cast<uint8_t>((g_back_idx + 1) % kNumFb);
    return true;
}

void dump_stats() {
    ESP_LOGI(TAG, "stats: submits=%lu, frame=%lld µs, wait=%lu enc=%lu sub=%lu µs",
             static_cast<unsigned long>(g_submits), frame_duration_us(),
             static_cast<unsigned long>(g_wait_us), static_cast<unsigned long>(g_encode_us),
             static_cast<unsigned long>(g_submit_us));
}

DebugCounters get_debug_counters() {
    DebugCounters c{};
    // Loop transmissions generate no DMA EOF and no LCD vsync — submit count
    // is the only meaningful per-frame counter on this backend.
    c.trans_done = g_submits;
    c.vsync      = g_submits;
    c.msync_err  = 0;
    c.wait_us    = g_wait_us;
    c.encode_us  = g_encode_us;
    c.submit_us  = g_submit_us;
    return c;
}

void wait_idle() {
    // A loop transmission never idles by design; the closest equivalent is
    // "the most recently submitted frame is now the one on the wire".
    if (g_prev_submit_idx >= 0) wait_back_buffer_free(static_cast<uint8_t>(g_prev_submit_idx));
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

}  // namespace pixfrog::output

#endif  // CONFIG_PIXFROG_LED_OUTPUT_PARLIO
