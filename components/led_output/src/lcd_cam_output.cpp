// LCD_CAM 16-bit free-running output driver — ESP32-P4.
//
// Uses esp_lcd_new_rgb_panel in `refresh_on_demand` mode with two frame
// buffers in PSRAM. render_frame() encodes the next frame into the back FB
// while the previous frame is still draining through GDMA, then waits on
// done_sem and kicks the new emission — CPU encode and DMA emission overlap,
// so the frame rate is bounded by max(encode, emission), not their sum.
//
// Frame geometry: the P4 LCD_CAM horizontal timing registers are 12 bits
// (lcd_ha_width / lcd_ht_width) and the vertical ones 10 bits, so a frame is
// laid out as v_res lines of h_res samples — h_res ≤ 4095 and v_res ≤ 1023,
// NOT one giant line (that silently truncates in hardware, validated on a
// Saleae 2026-06). The FB stays one flat sample array; the line split only
// exists in the panel timing. Hardware inserts one blank PCLK (the HSYNC
// tick) between lines; h_res is chosen as a multiple of every active NRZ bit
// period so that tick always lands on an inter-bit LOW and merely stretches
// it by 62.5 ns. The panel is torn down and recreated when a config commit
// changes the geometry; this runs off the hot path, never per steady-state
// frame.

#include "sdkconfig.h"
#if !CONFIG_PIXFROG_LED_OUTPUT_PARLIO

#include "led_output.h"

#include <cstring>

#include "driver/gpio.h"
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
#include "output_common.h"

namespace pixfrog::output {

namespace {

using common::desc_for_channel;
using common::lcm;
using common::required_frame_samples;
using common::round_up;

constexpr const char* TAG = "LCD_CAM";

InitConfig g_cfg{};
esp_lcd_panel_handle_t g_panel = nullptr;
void* g_fb_a                   = nullptr;
void* g_fb_b                   = nullptr;
size_t g_fb_bytes              = 0;
size_t g_fb_h_res              = 0;         // samples per line
size_t g_fb_v_res              = 0;         // lines per frame
size_t g_fb_samples            = 0;         // h_res × v_res = FB capacity in samples
uint8_t g_back_idx             = 0;         // which fb is next to write into (0 → write fb_a)
size_t g_written[2]            = { 0, 0 };  // samples encoded into each FB since creation

SemaphoreHandle_t g_done_sem = nullptr;

// Persistent calibration mode (TODO B5). -1 = normal pixel rendering.
volatile int8_t g_cal_mode = -1;

// ── Item 1: HSYNC pulse width and porches ────────────────────────────────────
// The HSYNC pin is never routed externally (hsync_gpio_num = -1), but the
// blanking region it defines is load-bearing: with zero porches the P4 LCD
// engine emits exactly one line and never re-arms the next (measured on a
// Saleae — 204-bit bursts, VSYNC_END never fires). Give it one PCLK of back
// and front porch. Total inter-line gap = hsw+hbp+hfp = 3 PCLK = 187.5 ns of
// LOW, which line_samples_for_config() keeps between NRZ bits.
constexpr int kHsyncPulseWidth = 1;
constexpr int kHsyncBackPorch  = 1;
constexpr int kHsyncFrontPorch = 1;

// ── Item 2: thresholds for swap latency warning ──────────────────────────────
// A no-copy FB swap inside esp_lcd_panel_draw_bitmap should return in
// well under 100 µs (just pointer juggling + cache barrier). If it's
// slower, either the IDF is doing a copy (config mistake) or we're
// blocked on a previous emission still in flight.
constexpr int64_t kSwapLatencyWarnUs = 200;

// Hardware ceilings of the P4 LCD_CAM RGB timing engine (lcd_cam_struct.h):
// ht_width = hsw + hbp + active + hfp - 1 must fit its 12-bit field, so the
// porch budget comes off the per-line active ceiling; vt_height is 10 bits.
constexpr size_t kMaxSamplesPerLine = 4095 - kHsyncPulseWidth - kHsyncBackPorch - kHsyncFrontPorch +
                                      1;
constexpr size_t kMaxLinesPerFrame = 1023;  // lcd_va_height/lcd_vt_height: 10 bits

// esp_cache_msync requires cache-line-granular sizes for C2M. The P4 L2
// line is 64 B or 128 B depending on CONFIG_CACHE_L2_CACHE_LINE_SIZE;
// 128 satisfies both.
constexpr size_t kCacheLineBytes = 128;

// Calibration frames are decoupled from the LED config: 262144 samples
// = 16.4 ms at PCLK=16 MHz, so one kick per 60 Hz render tick keeps the
// scope pattern essentially gapless (the 62.5 ns inter-line blank is
// invisible at scope timescales).
constexpr size_t kCalFrameSamples = 262144;

// ── Item 4: emission pacing ──────────────────────────────────────────────────
// Only on_vsync (the VSYNC_END interrupt = LCD engine finished clocking the
// frame out) may give done_sem. on_color_trans_done is NOT an emission
// signal: in refresh_on_demand mode without bounce buffers, the IDF driver
// invokes it synchronously inside esp_lcd_panel_draw_bitmap ("draw buffer
// handed over") — pacing on it lets the render loop outrun the LCD and each
// panel_refresh then cuts the tail off the in-flight frame (and VSYNC_END
// never fires). It is still registered, counter-only, for telemetry.
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

volatile uint32_t g_msync_err_count = 0;

void checked_msync(void* addr, size_t bytes) {
    const esp_err_t err = esp_cache_msync(addr, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (err != ESP_OK) {
        g_msync_err_count = g_msync_err_count + 1;
        ESP_LOGE(TAG, "esp_cache_msync(%p, %zu): %s", addr, bytes, esp_err_to_name(err));
    }
}

bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t /*panel*/,
                             const esp_lcd_rgb_panel_event_data_t* /*edata*/, void* /*user_ctx*/) {
    // Telemetry only — see Item 4. Plain read-modify-write: C++20 deprecates
    // ++ on a volatile lvalue.
    g_trans_done_count = g_trans_done_count + 1;
    return false;
}

bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t /*panel*/,
                        const esp_lcd_rgb_panel_event_data_t* /*edata*/, void* /*user_ctx*/) {
    g_vsync_count = g_vsync_count + 1;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_done_sem, &hp);
    return hp == pdTRUE;
}

// Samples per line for the current config. The hardware inserts one blank
// PCLK between lines; by making the line a multiple of every active NRZ bit
// period, that blank always falls between bits — where the wire is LOW for
// both bit values — and only stretches the low tail by 62.5 ns, which WS281x
// receivers don't care about. Clocked (APA/SK/LPD) and DMX channels tolerate
// the pause at any offset: their clock pauses together with the data, and a
// 62.5 ns stretch inside a 4 µs DMX bit is noise.
size_t line_samples_for_config() {
    size_t step = 2;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc = config::get_channel(ch);
        if (led::is_off(cc.protocol) || led::is_dmx(cc.protocol) || led::is_clocked(cc.protocol))
            continue;
        const led::Timing t = led::timing_for(cc.protocol, cc.clock_hz);
        if (t.samples_bit == 0) continue;
        const size_t merged = lcm(step, t.samples_bit);
        if (merged > kMaxSamplesPerLine) {
            // No common multiple fits one line; the odd channel out gets its
            // blank mid-bit (worst case a 62.5 ns notch). Keep the others.
            ESP_LOGW(TAG, "NRZ bit periods have no common line multiple <= %zu (ch%zu bit=%u)",
                     kMaxSamplesPerLine, ch, t.samples_bit);
            continue;
        }
        step = merged;
    }
    return kMaxSamplesPerLine / step * step;
}

void destroy_panel() {
    if (!g_panel) return;
    esp_lcd_panel_del(g_panel);
    g_panel        = nullptr;
    g_fb_a         = nullptr;
    g_fb_b         = nullptr;
    g_fb_bytes     = 0;
    g_fb_h_res     = 0;
    g_fb_v_res     = 0;
    g_fb_samples   = 0;
    g_last_kick_us = 0;
}

bool create_panel(size_t h_res, size_t v_res) {
    g_fb_h_res   = h_res;
    g_fb_v_res   = v_res;
    g_fb_samples = h_res * v_res;
    g_fb_bytes   = g_fb_samples * sizeof(uint16_t);

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
    panel_config.timings.v_res                 = g_fb_v_res;
    panel_config.timings.hsync_pulse_width     = kHsyncPulseWidth;
    panel_config.timings.hsync_back_porch      = kHsyncBackPorch;
    panel_config.timings.hsync_front_porch     = kHsyncFrontPorch;
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
    checked_msync(g_fb_a, round_up(g_fb_bytes, kCacheLineBytes));
    checked_msync(g_fb_b, round_up(g_fb_bytes, kCacheLineBytes));

    g_back_idx   = 0;
    g_written[0] = 0;
    g_written[1] = 0;

    ESP_LOGI(TAG,
             "panel up: fb_a=%p fb_b=%p, %zu bytes each, %zux%zu samples @ %lu Hz "
             "(%llu µs/frame)",
             g_fb_a, g_fb_b, g_fb_bytes, g_fb_h_res, g_fb_v_res,
             static_cast<unsigned long>(g_cfg.pclk_hz),
             static_cast<unsigned long long>(g_fb_samples + g_fb_v_res) * 1'000'000ULL /
                 g_cfg.pclk_hz);
    return true;
}

// Recreate the panel iff `needed_samples` changes the line/lines geometry.
// Holds done_sem across the swap so no emission is in flight while the FBs
// are reallocated. No-op (and cheap) in the steady state.
bool ensure_frame_capacity(size_t needed_samples) {
    if (needed_samples > g_cfg.max_samples_per_frame) {
        ESP_LOGE(TAG, "frame needs %zu samples > cap %lu — config should not have validated",
                 needed_samples, static_cast<unsigned long>(g_cfg.max_samples_per_frame));
        return false;
    }
    const size_t h_res = line_samples_for_config();
    const size_t v_res = (needed_samples + h_res - 1) / h_res;
    if (v_res > kMaxLinesPerFrame) {
        ESP_LOGE(TAG, "frame needs %zu lines of %zu samples > cap %zu", v_res, h_res,
                 kMaxLinesPerFrame);
        return false;
    }
    if (g_panel && h_res == g_fb_h_res && v_res == g_fb_v_res) return true;

    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "reconfigure: previous emission never drained");
        return false;
    }
    destroy_panel();
    const bool ok = create_panel(h_res, v_res);
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
                                             g_fb_samples);
    if (written == 0) {
        ESP_LOGE(TAG, "encode_frame wrote nothing (needed=%zu capacity=%zu)", needed, g_fb_samples);
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
    // GDMA could fetch stale data and emit garbage. g_fb_bytes need not be a
    // cache-line multiple (h_res follows protocol bit periods), so the last
    // flush may overrun the FB by < one line — that only writes back valid
    // adjacent heap data, never corrupts it.
    const size_t sync_bytes = round_up(sync_samples * sizeof(uint16_t), kCacheLineBytes);
    checked_msync(samples, sync_bytes);

    // Only now wait for the previous emission to drain: the encode above
    // worked on the FB that GDMA was NOT reading.
    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        dmx::note_dma_underrun();
        // Self-heal: timeout_ms is far beyond one emission, so the frame is
        // over even if VSYNC_END got lost — re-arm instead of bricking every
        // subsequent frame.
        xSemaphoreGive(g_done_sem);
        return false;
    }

    // Item 2: measure swap latency to confirm IDF is doing a no-copy swap.
    const int64_t t_kick_pre = esp_timer_get_time();

    // Hand the back FB to the panel as the next frame to emit. With num_fbs=2
    // and `refresh_on_demand`, IDF detects that the passed buffer matches one
    // of its internal FBs and simply switches the active pointer (no copy).
    esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, 0, static_cast<int>(g_fb_h_res),
                                              static_cast<int>(g_fb_v_res), samples);

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

    // In refresh_on_demand mode draw_bitmap only swaps the FB pointer — the
    // LCD engine is NOT started until this explicit kick. Without it nothing
    // is ever clocked out of the pins (on_color_trans_done still fires: it
    // is raised synchronously inside draw_bitmap and only means "draw buffer
    // handed over", not "emission done" — on_vsync is the emission signal).
    err = esp_lcd_rgb_panel_refresh(g_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_refresh: %s", esp_err_to_name(err));
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
    if (pattern_id == 3) {
        common::gpio_bitbang_probe_tick(g_cfg.bus_gpio_16);
        return true;
    }

    if (!ensure_frame_capacity(kCalFrameSamples)) return false;

    void* back        = (g_back_idx == 0) ? g_fb_a : g_fb_b;
    uint16_t* samples = static_cast<uint16_t*>(back);

    common::fill_calibration_pattern(samples, g_fb_samples, pattern_id, g_cfg.pclk_hz);
    g_written[g_back_idx] = g_fb_samples;

    checked_msync(samples, round_up(g_fb_bytes, kCacheLineBytes));

    if (xSemaphoreTake(g_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(g_done_sem);  // same self-heal as render_frame
        return false;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, 0, static_cast<int>(g_fb_h_res),
                                              static_cast<int>(g_fb_v_res), samples);
    if (err != ESP_OK) {
        xSemaphoreGive(g_done_sem);
        return false;
    }
    if ((err = esp_lcd_rgb_panel_refresh(g_panel)) != ESP_OK) {
        ESP_LOGE(TAG, "panel_refresh: %s", esp_err_to_name(err));
        xSemaphoreGive(g_done_sem);
        return false;
    }
    g_back_idx ^= 1;
    return true;
}

void dump_stats() {
    const int64_t expected_us = static_cast<int64_t>(g_fb_samples + g_fb_v_res) * 1'000'000LL /
                                g_cfg.pclk_hz;
    const int64_t avg_us = (g_emit_us_count > 0) ? (g_emit_us_sum / g_emit_us_count) : 0;
    ESP_LOGI(TAG,
             "stats: trans_done=%lu, vsync=%lu, frames=%lu, "
             "expected DMA=%lld µs, avg interval=%lld µs, max=%lld µs",
             static_cast<unsigned long>(g_trans_done_count),
             static_cast<unsigned long>(g_vsync_count), static_cast<unsigned long>(g_emit_us_count),
             expected_us, avg_us, g_emit_us_max);
}

DebugCounters get_debug_counters() {
    DebugCounters c{};
    c.trans_done = g_trans_done_count;
    c.vsync      = g_vsync_count;
    c.msync_err  = g_msync_err_count;
    return c;
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

}  // namespace pixfrog::output

#endif  // !CONFIG_PIXFROG_LED_OUTPUT_PARLIO
