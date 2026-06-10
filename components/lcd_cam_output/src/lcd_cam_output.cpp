// LCD_CAM 16-bit free-running output driver — ESP32-P4.
//
// Uses esp_lcd_new_rgb_panel in `refresh_on_demand` mode with two frame
// buffers in PSRAM. render_frame() encodes the next frame into the back FB
// while the previous frame is still draining through GDMA, then waits on
// done_sem and kicks the new emission — CPU encode and DMA emission overlap,
// so the frame rate is bounded by max(encode, emission), not their sum.
//
// The panel's h_res (= emission duration) tracks the frame length actually
// required by the current channel config — longest channel incl. reset tail —
// not the theoretical worst case. The panel is torn down and recreated when a
// config commit moves that length to a different bucket; this runs off the
// hot path (once per UI commit), never per steady-state frame.
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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"

namespace pixfrog::lcd {

namespace {

constexpr const char* TAG = "LCD_CAM";

InitConfig g_cfg{};
esp_lcd_panel_handle_t g_panel = nullptr;
void* g_fb_a                   = nullptr;
void* g_fb_b                   = nullptr;
size_t g_fb_bytes              = 0;
size_t g_fb_h_res              = 0;         // samples per "line" (= total samples per frame)
uint8_t g_back_idx             = 0;         // which fb is next to write into (0 → write fb_a)
size_t g_written[2]            = { 0, 0 };  // samples encoded into each FB since creation

SemaphoreHandle_t g_done_sem = nullptr;

// Persistent calibration mode (TODO B5). -1 = normal pixel rendering.
volatile int8_t g_cal_mode = -1;

// ── Item 1: HSYNC pulse width ────────────────────────────────────────────────
// ESP-IDF v5.3 RGB panel driver rejects hsync_pulse_width == 0 on ESP32-P4
// and forces it to >= 1. We use 1 (= 62.5 ns at PCLK=16MHz). The HSYNC pin
// is never routed externally (hsync_gpio_num = -1) so this is harmless.
// If hardware validation shows this 1-tick gap glitches WS2815 timing,
// raise to 2 and re-measure.
constexpr int kHsyncPulseWidth = 1;

// ── Item 2: thresholds for swap latency warning ──────────────────────────────
// A no-copy FB swap inside esp_lcd_panel_draw_bitmap should return in
// well under 100 µs (just pointer juggling + cache barrier). If it's
// slower, either the IDF is doing a copy (config mistake) or we're
// blocked on a previous emission still in flight.
constexpr int64_t kSwapLatencyWarnUs = 200;

// h_res granularity: 64 samples = 128 B = two 64-B cache lines. Keeps FB
// sizes cache-line aligned and avoids panel re-creation when a config tweak
// shifts the frame length by a few samples within the same bucket.
constexpr size_t kHresQuantum = 64;

// esp_cache_msync requires cache-line-granular sizes for C2M. The P4 L2
// line is 64 B or 128 B depending on CONFIG_CACHE_L2_CACHE_LINE_SIZE;
// 128 satisfies both.
constexpr size_t kCacheLineBytes = 128;

// Calibration frames are decoupled from the LED config: 262144 samples
// = 16.4 ms at PCLK=16 MHz, so one kick per 60 Hz render tick keeps the
// scope pattern essentially gapless.
constexpr size_t kCalFrameSamples = 262144;

// ── Item 4: callback identification counters ─────────────────────────────────
// Both on_color_trans_done and on_vsync are registered. Whichever fires
// first wakes done_sem. Counters help diagnose post-mortem which path was
// used in production (logged at shutdown / periodically).
volatile uint32_t g_trans_done_count = 0;
volatile uint32_t g_vsync_count      = 0;

// ── Item 5: bandwidth / emission timing telemetry ────────────────────────────
// Tracks the wall-clock duration between consecutive draw_bitmap kicks.
// At steady state, this should match the expected DMA duration
// (= h_res / pclk_hz) within a small margin. Larger deviation = PSRAM
// contention or scheduler jitter.
int64_t g_last_kick_us   = 0;
int64_t g_emit_us_sum    = 0;
uint32_t g_emit_us_count = 0;
int64_t g_emit_us_max    = 0;

bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t /*panel*/,
                             const esp_lcd_rgb_panel_event_data_t* /*edata*/, void* /*user_ctx*/) {
    // Plain read-modify-write: C++20 deprecates ++ on a volatile lvalue.
    g_trans_done_count = g_trans_done_count + 1;
    BaseType_t hp      = pdFALSE;
    xSemaphoreGiveFromISR(g_done_sem, &hp);
    return hp == pdTRUE;
}

bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t /*panel*/,
                        const esp_lcd_rgb_panel_event_data_t* /*edata*/, void* /*user_ctx*/) {
    g_vsync_count = g_vsync_count + 1;
    BaseType_t hp = pdFALSE;
    // Fallback: if on_color_trans_done never fires on this IDF/HW combo,
    // on_vsync still releases the sem so the pipeline keeps flowing.
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

size_t round_up(size_t v, size_t q) {
    return (v + q - 1) / q * q;
}

// Frame length the current config needs: the longest channel (incl. its
// reset tail). Channels share the bus in parallel, so this is a max, not a
// sum. Pure arithmetic — cheap enough to evaluate every frame.
size_t required_frame_samples() {
    size_t mx = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const led::ChannelDesc d = desc_for_channel(ch);
        if (led::is_off(d.protocol)) continue;
        const size_t len = led::encoded_size_samples(d);
        if (len > mx) mx = len;
    }
    return mx;
}

void destroy_panel() {
    if (!g_panel) return;
    esp_lcd_panel_del(g_panel);
    g_panel        = nullptr;
    g_fb_a         = nullptr;
    g_fb_b         = nullptr;
    g_fb_bytes     = 0;
    g_fb_h_res     = 0;
    g_last_kick_us = 0;
}

bool create_panel(size_t h_res) {
    g_fb_h_res = h_res;
    g_fb_bytes = h_res * sizeof(uint16_t);

    // ── Item 3: PSRAM sanity check ───────────────────────────────────────────
    // We need 2 × fb_bytes plus a comfortable headroom for cache lines, the
    // universe pool, and unforeseen allocations. Refuse if the largest
    // contiguous PSRAM block can't host one FB (IDF will internally request
    // a single contiguous allocation per FB).
    const size_t psram_largest_blk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (psram_largest_blk < g_fb_bytes) {
        ESP_LOGE(TAG, "PSRAM largest free block %zu < one FB %zu — fragmentation or too small",
                 psram_largest_blk, g_fb_bytes);
        return false;
    }

    esp_lcd_rgb_panel_config_t panel_config{};
    panel_config.data_width            = 16;
    panel_config.num_fbs               = 2;
    panel_config.bounce_buffer_size_px = 0;
    panel_config.clk_src               = LCD_CLK_SRC_DEFAULT;
    // GPIO_NUM_NC casts keep this valid on both IDF v5.5 (int fields) and
    // v6 (gpio_num_t fields).
    panel_config.disp_gpio_num  = GPIO_NUM_NC;
    panel_config.pclk_gpio_num  = GPIO_NUM_NC;
    panel_config.hsync_gpio_num = GPIO_NUM_NC;
    panel_config.vsync_gpio_num = GPIO_NUM_NC;
    panel_config.de_gpio_num    = GPIO_NUM_NC;
    for (int i = 0; i < 16; ++i) {
        panel_config.data_gpio_nums[i] = static_cast<gpio_num_t>(g_cfg.bus_gpio_16[i]);
    }
    panel_config.timings.pclk_hz               = g_cfg.pclk_hz;
    panel_config.timings.h_res                 = g_fb_h_res;
    panel_config.timings.v_res                 = 1;
    panel_config.timings.hsync_pulse_width     = kHsyncPulseWidth;
    panel_config.timings.hsync_back_porch      = 0;
    panel_config.timings.hsync_front_porch     = 0;
    panel_config.timings.vsync_pulse_width     = 1;
    panel_config.timings.vsync_back_porch      = 0;
    panel_config.timings.vsync_front_porch     = 0;
    panel_config.timings.flags.pclk_active_neg = 0;
    panel_config.flags.fb_in_psram             = 1;
    panel_config.flags.refresh_on_demand       = 1;
    panel_config.flags.bb_invalidate_cache     = 0;  // we manage cache ourselves

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &g_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return false;
    }

    // Item 4: register BOTH callbacks. On most ESP32-P4 IDF builds in
    // refresh_on_demand mode, on_color_trans_done is the canonical signal
    // of "DMA emission complete". Some builds only fire on_vsync. We
    // register both; whichever arrives first releases done_sem (second
    // give is a no-op on a binary sem). Counters tell us at runtime which
    // path was taken — `lcd_cam_dump_stats()` exposes them.
    esp_lcd_rgb_panel_event_callbacks_t cbs{};
    cbs.on_color_trans_done = on_trans_done;
    cbs.on_vsync            = on_vsync;
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
    esp_cache_msync(g_fb_a, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(g_fb_b, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    g_back_idx   = 0;
    g_written[0] = 0;
    g_written[1] = 0;

    ESP_LOGI(TAG, "panel up: fb_a=%p fb_b=%p, %zu bytes each, h_res=%zu @ %lu Hz (%llu µs/frame)",
             g_fb_a, g_fb_b, g_fb_bytes, g_fb_h_res, static_cast<unsigned long>(g_cfg.pclk_hz),
             static_cast<unsigned long long>(g_fb_h_res) * 1'000'000ULL / g_cfg.pclk_hz);
    return true;
}

// Recreate the panel iff `needed_samples` lands in a different h_res bucket.
// Holds done_sem across the swap so no emission is in flight while the FBs
// are reallocated. No-op (and cheap) in the steady state.
bool ensure_frame_capacity(size_t needed_samples) {
    if (needed_samples > g_cfg.max_samples_per_frame) {
        ESP_LOGE(TAG, "frame needs %zu samples > cap %lu — config should not have validated",
                 needed_samples, static_cast<unsigned long>(g_cfg.max_samples_per_frame));
        return false;
    }
    const size_t h_res = round_up(needed_samples, kHresQuantum);
    if (g_panel && h_res == g_fb_h_res) return true;

    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "reconfigure: previous emission never drained");
        return false;
    }
    destroy_panel();
    const bool ok = create_panel(h_res);
    xSemaphoreGive(g_done_sem);
    return ok;
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

    g_done_sem = xSemaphoreCreateBinary();
    if (!g_done_sem) return false;
    xSemaphoreGive(g_done_sem);  // start idle so the first render_frame doesn't block

    // All channels Off at boot is legal: the panel is created lazily by the
    // first frame that has something to emit.
    const size_t needed = required_frame_samples();
    if (needed > 0 && !ensure_frame_capacity(needed)) return false;

    ESP_LOGI(TAG, "init OK (%s)", needed > 0 ? "panel created" : "all channels off, panel lazy");
    return true;
}

bool render_frame(uint32_t timeout_ms) {
    const size_t needed = required_frame_samples();
    if (needed == 0) return true;  // every channel Off — nothing to emit
    if (!ensure_frame_capacity(needed)) return false;

    void* back        = (g_back_idx == 0) ? g_fb_a : g_fb_b;
    uint16_t* samples = static_cast<uint16_t*>(back);

    led::ChannelDesc descs[config::kNumChannels];
    const uint8_t* pixels[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        descs[ch]  = desc_for_channel(ch);
        pixels[ch] = dmx::pixel_front_buffer(ch);
    }

    // Single pass, pure stores: [0, written) is fully initialized without a
    // prior memset of the FB, and without per-channel read-modify-write
    // traversals. This runs while the previous frame is still emitting from
    // the other FB — the overlap is what holds 60 FPS.
    const size_t written = led::encode_frame(descs, pixels, config::kNumChannels, samples,
                                             g_fb_h_res);
    if (written == 0) {
        ESP_LOGE(TAG, "encode_frame wrote nothing (needed=%zu h_res=%zu)", needed, g_fb_h_res);
        return false;
    }

    // If the frame shrank within the same h_res bucket, this FB still holds
    // stale samples from its previous encode past `written` — zero the gap so
    // the tail stays TRESET-compliant LOW.
    size_t sync_samples = written;
    if (g_written[g_back_idx] > written) {
        std::memset(samples + written, 0, (g_written[g_back_idx] - written) * sizeof(uint16_t));
        sync_samples = g_written[g_back_idx];
    }
    g_written[g_back_idx] = written;

    // Make the CPU writes visible to GDMA — but only the bytes this frame
    // touched, not the whole FB. PSRAM cache is write-back; without this,
    // GDMA could fetch stale data and emit garbage.
    size_t sync_bytes = round_up(sync_samples * sizeof(uint16_t), kCacheLineBytes);
    if (sync_bytes > g_fb_bytes) sync_bytes = g_fb_bytes;
    esp_cache_msync(samples, sync_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // Only now wait for the previous emission to drain: the encode above
    // worked on the FB that GDMA was NOT reading.
    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        dmx::note_dma_underrun();
        return false;
    }

    // Item 2: measure swap latency to confirm IDF is doing a no-copy swap.
    const int64_t t_kick_pre = esp_timer_get_time();

    // Hand the back FB to the panel as the next frame to emit. With num_fbs=2
    // and `refresh_on_demand`, IDF detects that the passed buffer matches one
    // of its internal FBs and simply switches the active pointer (no copy).
    esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, 0, static_cast<int>(g_fb_h_res), 1,
                                              samples);

    const int64_t t_kick_post = esp_timer_get_time();
    const int64_t swap_us     = t_kick_post - t_kick_pre;
    if (swap_us > kSwapLatencyWarnUs) {
        ESP_LOGW(TAG,
                 "draw_bitmap took %lld µs (expected <%lld for no-copy swap); "
                 "check IDF version, num_fbs=2, fb_in_psram=1",
                 swap_us, kSwapLatencyWarnUs);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        xSemaphoreGive(g_done_sem);
        return false;
    }

    // Item 5: track inter-kick interval. At steady-state this equals the
    // wall-clock period of the render task, which itself is bounded below
    // by the DMA emission duration.
    if (g_last_kick_us != 0) {
        const int64_t interval  = t_kick_post - g_last_kick_us;
        g_emit_us_sum          += interval;
        g_emit_us_count++;
        if (interval > g_emit_us_max) g_emit_us_max = interval;
    }
    g_last_kick_us = t_kick_post;

    g_back_idx ^= 1;
    dmx::note_frame_emitted();
    return true;
}

bool emit_calibration_pattern(uint8_t pattern_id) {
    if (!ensure_frame_capacity(kCalFrameSamples)) return false;

    void* back        = (g_back_idx == 0) ? g_fb_a : g_fb_b;
    uint16_t* samples = static_cast<uint16_t*>(back);

    switch (pattern_id) {
    case 0: {
        // 1 kHz square wave on all 16 bits: half-period = pclk/1000/2 samples.
        const size_t half = g_cfg.pclk_hz / 2000;
        for (size_t i = 0; i < g_fb_h_res; ++i) {
            const bool high = ((i / half) & 1) == 0;
            samples[i]      = high ? 0xFFFFu : 0x0000u;
        }
        break;
    }
    case 1: {
        // Walking-1 across the 16 bits, holding each high for 256 samples
        // (= 16 µs at PCLK=16 MHz — comfortably scope-able).
        constexpr size_t per_bit = 256;
        for (size_t i = 0; i < g_fb_h_res; ++i) {
            samples[i] = static_cast<uint16_t>(1u << ((i / per_bit) % 16));
        }
        break;
    }
    case 2: {
        // 0xAAAA on even samples, 0x5555 on odd samples — every sample
        // flips every bit, useful to see if PCLK is correct.
        for (size_t i = 0; i < g_fb_h_res; ++i) {
            samples[i] = (i & 1) ? 0x5555u : 0xAAAAu;
        }
        break;
    }
    default: std::memset(samples, 0, g_fb_bytes); break;
    }
    g_written[g_back_idx] = g_fb_h_res;

    esp_cache_msync(samples, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, 0, static_cast<int>(g_fb_h_res), 1,
                                              samples);
    if (err != ESP_OK) {
        xSemaphoreGive(g_done_sem);
        return false;
    }
    g_back_idx ^= 1;
    return true;
}

void dump_stats() {
    const int64_t expected_us = static_cast<int64_t>(g_fb_h_res) * 1'000'000LL / g_cfg.pclk_hz;
    const int64_t avg_us      = (g_emit_us_count > 0) ? (g_emit_us_sum / g_emit_us_count) : 0;
    ESP_LOGI(TAG,
             "stats: trans_done=%lu, vsync=%lu, frames=%lu, "
             "expected DMA=%lld µs, avg interval=%lld µs, max=%lld µs",
             static_cast<unsigned long>(g_trans_done_count),
             static_cast<unsigned long>(g_vsync_count), static_cast<unsigned long>(g_emit_us_count),
             expected_us, avg_us, g_emit_us_max);
}

void wait_idle() {
    if (g_done_sem && xSemaphoreTake(g_done_sem, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(g_done_sem);
    }
}

size_t fb_bytes() {
    return g_fb_bytes;
}

void set_calibration_mode(int8_t pattern_id) {
    g_cal_mode = pattern_id;
}
int8_t get_calibration_mode() {
    return g_cal_mode;
}

}  // namespace pixfrog::lcd
