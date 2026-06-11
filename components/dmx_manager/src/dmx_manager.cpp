#include "dmx_manager.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "dmx_logic.h"
#include "led_protocols.h"

namespace pixfrog::dmx {

namespace {

constexpr const char* TAG = "DMX";

// Two universe banks, each `kNumUniverses * kUniverseSize` bytes, in PSRAM.
uint8_t* g_uni_bank_a = nullptr;
uint8_t* g_uni_bank_b = nullptr;
std::atomic<uint8_t*> g_uni_front{ nullptr };
uint8_t* g_uni_back = nullptr;

// Per-channel pixel buffers in internal SRAM, double-buffered.
struct ChanBufs {
    uint8_t* a;
    uint8_t* b;
    std::atomic<uint8_t*> front;
    uint8_t* back;
};
ChanBufs g_chan_bufs[config::kNumChannels]{};

Stats g_stats{};
std::atomic<bool> g_sync_pending{ false };

// Pixel-count preview state: channel (high 16 bits) + count (low 16 bits)
// packed into one atomic so render_task always reads a consistent pair.
// 0xFFFF in the channel half = preview inactive.
constexpr uint32_t kPreviewOff = 0xFFFF0000u;
std::atomic<uint32_t> g_pixel_preview{ kPreviewOff };

// Active standalone scene (-1 = none).
std::atomic<int8_t> g_active_scene{ -1 };

// Identify blink: channel in the high byte (0xFF = off), expiry in ms since
// boot in the low 24 bits won't fit — use two relaxed atomics; tearing across
// them costs at most one oddly-timed frame.
std::atomic<int8_t> g_identify_ch{ -1 };
std::atomic<uint32_t> g_identify_until_ms{ 0 };

// Map a universe number to a slot in the bank. For v0 we use a simple
// flat allocation: universe N → slot (N % kNumUniverses). On boot, the
// config_store tells us which universes are routed to which channel; we
// keep this LUT for fast lookup.
//
// TODO(v1): replace with a hash map keyed by (net, subnet, universe).
uint16_t g_universe_to_slot[32768]{};
bool g_universe_to_slot_valid = false;

// Reverse mapping slot → channel index, populated alongside g_universe_to_slot.
uint8_t g_slot_to_channel[kNumUniverses]{};
uint16_t g_slots_used = 0;

// 2-source merge: per-slot source tracking + per-source staging frames
// (2 × kUniverseSize per slot, PSRAM). Receiver tasks (artnet + sacn, same
// priority, core 0) may interleave here at tick boundaries; like the bank
// writes, a torn merge costs at most one oddly-blended frame.
logic::MergeState g_merge[kNumUniverses]{};
uint8_t* g_merge_staging = nullptr;

// Per-channel last-activity timestamp (µs). 0 = never seen.
int64_t g_last_activity_us[config::kNumChannels]{};
// "Active" if last_activity within this window:
constexpr int64_t kActivityWindowUs = 1'000'000;  // 1 second

// Per-channel capacity flag (item 2). True if the channel's encoded frame
// fits within one refresh period. Defaults to true (assumed OK until
// proven otherwise by validate_capacity).
bool g_channel_capacity_ok[config::kNumChannels]{};

// Event group for config change propagation (item 7).
EventGroupHandle_t g_remap_eg = nullptr;

// Binary semaphore for ArtSync → render_task fast-path (TODO B2).
SemaphoreHandle_t g_sync_sem = nullptr;

// Rebuild the universe → slot LUT (+ reverse slot → channel) from the
// current contents of config_store. Called from init() and from
// handle_pending_remaps() when the UI signals a config change.
void rebuild_universe_lut() {
    for (size_t i = 0; i < sizeof(g_universe_to_slot) / sizeof(g_universe_to_slot[0]); ++i) {
        g_universe_to_slot[i] = UINT16_MAX;
    }
    uint16_t slot = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc              = config::get_channel(ch);
        const size_t universes_used = logic::channel_universes_used(cc);
        for (size_t u = 0; u < universes_used && slot < kNumUniverses; ++u) {
            g_universe_to_slot[cc.universe_start + u] = slot;
            g_slot_to_channel[slot]                   = static_cast<uint8_t>(ch);
            slot++;
        }
    }
    g_slots_used = slot;
    // Slots may now mean different universes — tracked sources are stale.
    std::memset(g_merge, 0, sizeof(g_merge));
}

}  // namespace

bool init() {
    const size_t bank_bytes = kNumUniverses * kUniverseSize;
    g_uni_bank_a = static_cast<uint8_t*>(heap_caps_calloc(1, bank_bytes, MALLOC_CAP_SPIRAM));
    g_uni_bank_b = static_cast<uint8_t*>(heap_caps_calloc(1, bank_bytes, MALLOC_CAP_SPIRAM));
    if (!g_uni_bank_a || !g_uni_bank_b) {
        ESP_LOGE(TAG, "PSRAM alloc for universe banks failed");
        return false;
    }
    g_uni_front.store(g_uni_bank_a, std::memory_order_release);
    g_uni_back = g_uni_bank_b;

    g_merge_staging = static_cast<uint8_t*>(
        heap_caps_calloc(1, kNumUniverses * 2 * kUniverseSize, MALLOC_CAP_SPIRAM));
    if (!g_merge_staging) {
        ESP_LOGE(TAG, "PSRAM alloc for merge staging failed");
        return false;
    }

    for (size_t i = 0; i < config::kNumChannels; ++i) {
        g_chan_bufs[i].a = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        g_chan_bufs[i].b = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!g_chan_bufs[i].a || !g_chan_bufs[i].b) {
            ESP_LOGE(TAG, "SRAM alloc for channel %u failed", static_cast<unsigned>(i));
            return false;
        }
        g_chan_bufs[i].front.store(g_chan_bufs[i].a, std::memory_order_release);
        g_chan_bufs[i].back = g_chan_bufs[i].b;
    }

    rebuild_universe_lut();
    g_universe_to_slot_valid = true;

    g_remap_eg = xEventGroupCreate();
    if (!g_remap_eg) {
        ESP_LOGE(TAG, "event group alloc failed");
        return false;
    }

    g_sync_sem = xSemaphoreCreateBinary();
    if (!g_sync_sem) {
        ESP_LOGE(TAG, "sync semaphore alloc failed");
        return false;
    }

    for (size_t i = 0; i < config::kNumChannels; ++i)
        g_channel_capacity_ok[i] = true;
    validate_capacity();

    ESP_LOGI(TAG, "init OK, universe LUT built");
    return true;
}

void mark_channel_dirty(size_t channel_index) {
    if (channel_index >= config::kNumChannels || !g_remap_eg) return;
    xEventGroupSetBits(g_remap_eg, 1u << channel_index);
}

void mark_global_dirty() {
    if (!g_remap_eg) return;
    xEventGroupSetBits(g_remap_eg, kRemapGlobalBit);
}

void handle_pending_remaps() {
    if (!g_remap_eg) return;
    // Only bits 0..8 are ever set (channels + global). Clearing the reserved
    // top byte (0xFF000000) trips a FreeRTOS assert in xEventGroupClearBits.
    const EventBits_t bits = xEventGroupClearBits(g_remap_eg,
                                                  kRemapAllChannelsMask | kRemapGlobalBit);
    if (!bits) return;
    // Any channel bit (or global) currently triggers a full LUT rebuild.
    // The LUT covers 32k entries × 2 B = 64 kB so the rebuild is cheap
    // (~100 µs) and runs once per UI commit — not per frame.
    if (bits & (kRemapAllChannelsMask | kRemapGlobalBit)) {
        rebuild_universe_lut();
        validate_capacity();
    }
    ESP_LOGI(TAG, "remap applied, bits=0x%lx", static_cast<unsigned long>(bits));
}

void set_pixel_preview(size_t channel_index, uint16_t pixel_count) {
    if (channel_index >= config::kNumChannels) return;
    g_pixel_preview.store((static_cast<uint32_t>(channel_index) << 16) | pixel_count,
                          std::memory_order_relaxed);
}

void clear_pixel_preview() {
    g_pixel_preview.store(kPreviewOff, std::memory_order_relaxed);
}

int pixel_preview_channel() {
    const uint32_t v  = g_pixel_preview.load(std::memory_order_relaxed);
    const uint32_t ch = v >> 16;
    return ch == 0xFFFFu ? -1 : static_cast<int>(ch);
}

uint16_t pixel_preview_count() {
    return static_cast<uint16_t>(g_pixel_preview.load(std::memory_order_relaxed) & 0xFFFFu);
}

void identify_start(size_t channel_index, uint16_t seconds) {
    if (channel_index >= config::kNumChannels) return;
    g_identify_until_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000) + seconds * 1000u,
                              std::memory_order_relaxed);
    g_identify_ch.store(static_cast<int8_t>(channel_index), std::memory_order_relaxed);
}

void identify_stop() {
    g_identify_ch.store(-1, std::memory_order_relaxed);
}

int identify_channel() {
    const int ch = g_identify_ch.load(std::memory_order_relaxed);
    if (ch < 0) return -1;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (static_cast<int32_t>(g_identify_until_ms.load(std::memory_order_relaxed) - now) <= 0) {
        g_identify_ch.store(-1, std::memory_order_relaxed);
        return -1;
    }
    return ch;
}

void scene_start(uint8_t scene_index) {
    if (scene_index >= config::kNumScenes) return;
    g_active_scene.store(static_cast<int8_t>(scene_index), std::memory_order_relaxed);
}

void scene_stop() {
    g_active_scene.store(-1, std::memory_order_relaxed);
}

int active_scene() {
    return g_active_scene.load(std::memory_order_relaxed);
}

bool decode_pixels_for_channel(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    uint8_t* dst = pixel_back_buffer(ch);
    if (!dst) return false;

    // Identify blink: top priority — it answers "which strip is this?".
    if (identify_channel() == static_cast<int>(ch)) {
        const auto& cc     = config::get_channel(ch);
        const uint8_t bpp  = led::bytes_per_pixel(cc.protocol);
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint8_t lvl  = ((now / 250) & 1) ? 255 : 0;  // 2 Hz blink
        logic::fill_failsafe_pattern(dst, kMaxBytesPerChan, cc.pixel_count, bpp,
                                     config::kFailsafeColor, lvl, lvl, lvl);
        return true;
    }

    const uint32_t preview = g_pixel_preview.load(std::memory_order_relaxed);
    if ((preview >> 16) == ch) {
        const auto& cc = config::get_channel(ch);
        logic::fill_preview_pattern(dst, kMaxBytesPerChan, static_cast<uint16_t>(preview & 0xFFFFu),
                                    led::bytes_per_pixel(cc.protocol));
        return true;
    }

    // Signal-loss failsafe: a channel silent past the timeout stops decoding
    // (stale) universes and emits the fallback instead. Hold mode never gets
    // here — stale decode IS the hold. DMX512 outputs degrade colour→blackout
    // (an RGB fill has no meaning on a generic universe).
    // Standalone scene: manual override — masked channels render the effect,
    // incoming traffic is ignored until scene_stop(). LED protocols only.
    const int sc = g_active_scene.load(std::memory_order_relaxed);
    if (sc >= 0) {
        const auto& scene = config::get_scene(static_cast<size_t>(sc));
        const auto& cc    = config::get_channel(ch);
        if (((scene.channel_mask >> ch) & 1) && !led::is_dmx(cc.protocol)) {
            logic::fill_scene_pattern(dst, kMaxBytesPerChan, cc.pixel_count,
                                      led::bytes_per_pixel(cc.protocol), scene.effect, scene.r,
                                      scene.g, scene.b, scene.speed, scene.param,
                                      static_cast<uint32_t>(esp_timer_get_time() / 1000));
            return true;
        }
    }

    const auto& g = config::get_global();
    if (g.failsafe_mode != config::kFailsafeHold &&
        logic::failsafe_due(g_last_activity_us[ch], esp_timer_get_time(), g.failsafe_timeout_s)) {
        const auto& cc = config::get_channel(ch);
        // Mode "scene": play the configured scene's effect on the lost channel.
        if (g.failsafe_mode == config::kFailsafeScene && !led::is_dmx(cc.protocol)) {
            const auto& scene = config::get_scene(g.failsafe_scene);
            logic::fill_scene_pattern(dst, kMaxBytesPerChan, cc.pixel_count,
                                      led::bytes_per_pixel(cc.protocol), scene.effect, scene.r,
                                      scene.g, scene.b, scene.speed, scene.param,
                                      static_cast<uint32_t>(esp_timer_get_time() / 1000));
            return true;
        }
        const uint8_t mode = led::is_dmx(cc.protocol) ? config::kFailsafeBlackout : g.failsafe_mode;
        logic::fill_failsafe_pattern(dst, kMaxBytesPerChan, cc.pixel_count,
                                     led::bytes_per_pixel(cc.protocol), mode, g.failsafe_r,
                                     g.failsafe_g, g.failsafe_b);
        return true;
    }

    return logic::decode_pixels(dst, kMaxBytesPerChan, config::get_channel(ch),
                                [](uint16_t u) { return universe_front_buffer_for(u); });
}

bool is_channel_capacity_ok(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    return g_channel_capacity_ok[ch];
}

void validate_capacity() {
    const uint8_t refresh = config::get_global().refresh_rate_hz;
    if (refresh == 0) return;
    const uint64_t allowance = logic::emission_budget_us(refresh);

    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc            = config::get_channel(ch);
        const bool ok             = logic::channel_fits_budget(cc, led::kPclkHz, allowance);
        g_channel_capacity_ok[ch] = ok;
        if (!ok) {
            const uint64_t t_us = logic::channel_t_dma_us(cc, led::kPclkHz);
            ESP_LOGW(TAG,
                     "ch %zu over capacity: t_dma=%llu µs > budget=%llu µs "
                     "(refresh=%u Hz, %u px, proto=%d) — reduce pixel_count or refresh",
                     ch, static_cast<unsigned long long>(t_us),
                     static_cast<unsigned long long>(allowance), static_cast<unsigned>(refresh),
                     static_cast<unsigned>(cc.pixel_count), static_cast<int>(cc.protocol));
        }
    }
}

int channel_for_universe(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return -1;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return -1;
    return static_cast<int>(g_slot_to_channel[slot]);
}

void note_channel_activity(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return;
    g_last_activity_us[channel_index] = esp_timer_get_time();
}

bool is_channel_active(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const int64_t last = g_last_activity_us[channel_index];
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < kActivityWindowUs;
}

bool is_channel_failsafe(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const auto& g = config::get_global();
    if (g.failsafe_mode == config::kFailsafeHold) return false;
    return logic::failsafe_due(g_last_activity_us[channel_index], esp_timer_get_time(),
                               g.failsafe_timeout_s);
}

void note_universe_terminated(uint16_t universe_number) {
    const int ch = channel_for_universe(universe_number);
    if (ch < 0) return;
    // Age the timestamp to the epoch+1µs: still "was active once", but past
    // any timeout — the next render tick applies the failsafe.
    if (g_last_activity_us[ch] != 0) g_last_activity_us[ch] = 1;
}

uint8_t* universe_back_buffer_for(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return nullptr;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return nullptr;
    return g_uni_back + slot * kUniverseSize;
}

bool write_universe_from_source(uint16_t universe_number, const uint8_t* data, size_t len,
                                uint32_t source_id, int64_t timeout_us) {
    if (!g_universe_to_slot_valid || !data || !g_merge_staging) return false;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return false;
    const bool ltp = config::get_global().merge_mode == config::kMergeLtp;
    return logic::merge_ingest(g_merge[slot],
                               g_merge_staging + static_cast<size_t>(slot) * 2 * kUniverseSize,
                               g_uni_back + static_cast<size_t>(slot) * kUniverseSize, data, len,
                               source_id, ltp, esp_timer_get_time(), timeout_us);
}

void merge_drop_source(uint16_t universe_number, uint32_t source_id) {
    if (!g_universe_to_slot_valid) return;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return;
    logic::merge_drop(g_merge[slot], source_id);
}

void merge_reset_universe(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return;
    std::memset(&g_merge[slot], 0, sizeof(g_merge[slot]));
}

void merge_cancel_all() {
    std::memset(g_merge, 0, sizeof(g_merge));
}

bool is_channel_merging(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const int64_t now = esp_timer_get_time();
    for (uint16_t slot = 0; slot < g_slots_used; ++slot) {
        if (g_slot_to_channel[slot] != channel_index) continue;
        logic::MergeState m = g_merge[slot];
        logic::merge_expire(m, now, kArtnetMergeTimeoutUs);
        if (logic::merge_active_count(m) == 2) return true;
    }
    return false;
}

const uint8_t* universe_front_buffer_for(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return nullptr;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return nullptr;
    return g_uni_front.load(std::memory_order_acquire) + slot * kUniverseSize;
}

bool inject_universe(uint16_t universe_number, size_t offset, const uint8_t* data, size_t len) {
    if (!g_universe_to_slot_valid || !data) return false;
    if (offset + len > kUniverseSize) return false;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return false;
    const size_t base = static_cast<size_t>(slot) * kUniverseSize + offset;
    // Byte-level tearing against a concurrent artnet write or render read is
    // acceptable here — this is a bench/test path, not a sync-critical one.
    memcpy(g_uni_bank_a + base, data, len);
    memcpy(g_uni_bank_b + base, data, len);
    note_channel_activity(g_slot_to_channel[slot]);
    return true;
}

void note_packet_rx() {
    __atomic_add_fetch(&g_stats.artnet_packets_rx, 1, __ATOMIC_RELAXED);
}
void note_packet_bad() {
    __atomic_add_fetch(&g_stats.artnet_bad_packets, 1, __ATOMIC_RELAXED);
}
void note_ctrl_rx() {
    __atomic_add_fetch(&g_stats.artnet_ctrl_rx, 1, __ATOMIC_RELAXED);
}
void note_sacn_rx() {
    __atomic_add_fetch(&g_stats.sacn_packets_rx, 1, __ATOMIC_RELAXED);
}
void note_sync() {
    g_sync_pending.store(true, std::memory_order_release);
    if (g_sync_sem) xSemaphoreGive(g_sync_sem);
}

bool wait_for_sync_or_period(uint32_t period_ticks) {
    if (!g_sync_sem) {
        vTaskDelay(period_ticks);
        return false;
    }
    return xSemaphoreTake(g_sync_sem, period_ticks) == pdTRUE;
}

void swap_universes() {
    uint8_t* new_front = g_uni_back;
    g_uni_back         = g_uni_front.exchange(new_front, std::memory_order_acq_rel);
    // After swap, copy the now-back into a clean state? No: we keep stale data
    // around so a channel that has not received an update keeps its previous
    // value. This matches stage-lighting conventions.
}

uint8_t* pixel_back_buffer(size_t ch) {
    if (ch >= config::kNumChannels) return nullptr;
    return g_chan_bufs[ch].back;
}

const uint8_t* pixel_front_buffer(size_t ch) {
    if (ch >= config::kNumChannels) return nullptr;
    return g_chan_bufs[ch].front.load(std::memory_order_acquire);
}

void swap_pixels(size_t ch) {
    if (ch >= config::kNumChannels) return;
    uint8_t* new_front   = g_chan_bufs[ch].back;
    g_chan_bufs[ch].back = g_chan_bufs[ch].front.exchange(new_front, std::memory_order_acq_rel);
}

Stats get_stats() {
    Stats s = g_stats;
    return s;
}

void set_current_fps(uint32_t fps) {
    g_stats.current_fps = fps;
}
void note_frame_emitted() {
    __atomic_add_fetch(&g_stats.frames_emitted, 1, __ATOMIC_RELAXED);
}
void note_dma_underrun() {
    __atomic_add_fetch(&g_stats.dma_underruns, 1, __ATOMIC_RELAXED);
}

}  // namespace pixfrog::dmx
