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
size_t g_fb_h_res              = 0;  // samples per "line" (= total samples per frame)
uint8_t g_back_idx             = 0;  // which fb is next to write into (0 → write fb_a)

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

}  // namespace

bool init(const InitConfig& cfg) {
    g_cfg = cfg;

    if (cfg.max_samples_per_frame == 0 || cfg.bus_gpio_16 == nullptr) {
        ESP_LOGE(TAG, "invalid InitConfig");
        return false;
    }

    g_fb_h_res = cfg.max_samples_per_frame;
    g_fb_bytes = g_fb_h_res * sizeof(uint16_t);

    // ── Item 3: PSRAM sanity check ───────────────────────────────────────────
    // We need 2 × fb_bytes plus a comfortable headroom for cache lines, the
    // universe pool, and unforeseen allocations. Refuse boot if the largest
    // contiguous PSRAM block can't host one FB (IDF will internally request
    // a single contiguous allocation per FB).
    const size_t psram_total       = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest_blk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t psram_required    = g_fb_bytes * 2 + (256u * 1024u);
    if (psram_total == 0) {
        ESP_LOGE(TAG, "no PSRAM detected; ESP32-P4 octal PSRAM is mandatory");
        return false;
    }
    if (psram_largest_blk < g_fb_bytes) {
        ESP_LOGE(TAG, "PSRAM largest free block %zu < one FB %zu — fragmentation or too small",
                 psram_largest_blk, g_fb_bytes);
        return false;
    }
    if (psram_total < psram_required) {
        ESP_LOGE(TAG, "PSRAM too small: have %zu B, need %zu B (2 FBs + 256 kB headroom)",
                 psram_total, psram_required);
        return false;
    }
    ESP_LOGI(TAG, "PSRAM check OK: %zu MB total, %zu MB largest free, need %zu MB for 2 FBs",
             psram_total >> 20, psram_largest_blk >> 20, (g_fb_bytes * 2) >> 20);

    esp_lcd_rgb_panel_config_t panel_config{};
    panel_config.data_width            = 16;
    panel_config.num_fbs               = 2;
    panel_config.bounce_buffer_size_px = 0;
    panel_config.clk_src               = LCD_CLK_SRC_DEFAULT;
    panel_config.disp_gpio_num         = -1;
    panel_config.pclk_gpio_num         = -1;
    panel_config.hsync_gpio_num        = -1;
    panel_config.vsync_gpio_num        = -1;
    panel_config.de_gpio_num           = -1;
    for (int i = 0; i < 16; ++i) {
        panel_config.data_gpio_nums[i] = cfg.bus_gpio_16[i];
    }
    panel_config.timings.pclk_hz               = cfg.pclk_hz;
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

    g_done_sem = xSemaphoreCreateBinary();
    if (!g_done_sem) return false;
    xSemaphoreGive(g_done_sem);  // start idle so the first render_frame doesn't block

    ESP_LOGI(TAG, "init OK: fb_a=%p fb_b=%p, %zu bytes each, h_res=%zu @ %lu Hz", g_fb_a, g_fb_b,
             g_fb_bytes, g_fb_h_res, static_cast<unsigned long>(cfg.pclk_hz));
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

    void* back        = (g_back_idx == 0) ? g_fb_a : g_fb_b;
    uint16_t* samples = static_cast<uint16_t*>(back);

    // Zero the buffer so unused bus bits stay LOW (TRESET-compliant tail) and
    // so encoder OR-semantics start clean.
    std::memset(samples, 0, g_fb_bytes);

    // Encode each channel. Channels share the buffer via OR on their bus bits.
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const led::ChannelDesc d = desc_for_channel(ch);
        const uint8_t* px        = dmx::pixel_front_buffer(ch);
        if (!px) continue;
        led::encode_channel(d, px, samples, g_fb_h_res);
    }

    // Make the CPU writes visible to GDMA. PSRAM cache is write-back; without
    // this, GDMA could fetch stale data and emit garbage.
    esp_cache_msync(samples, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

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
    if (!g_panel) return false;
    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) return false;

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

    esp_cache_msync(samples, g_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
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
