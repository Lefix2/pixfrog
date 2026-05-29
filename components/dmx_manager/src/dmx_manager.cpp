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
    const EventBits_t bits = xEventGroupClearBits(g_remap_eg, 0xFFFFFFFFu);
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

bool decode_pixels_for_channel(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    uint8_t* dst = pixel_back_buffer(ch);
    if (!dst) return false;
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

uint8_t* universe_back_buffer_for(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return nullptr;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return nullptr;
    return g_uni_back + slot * kUniverseSize;
}

const uint8_t* universe_front_buffer_for(uint16_t universe_number) {
    if (!g_universe_to_slot_valid) return nullptr;
    const uint16_t slot = g_universe_to_slot[universe_number];
    if (slot == UINT16_MAX) return nullptr;
    return g_uni_front.load(std::memory_order_acquire) + slot * kUniverseSize;
}

void note_packet_rx() {
    __atomic_add_fetch(&g_stats.artnet_packets_rx, 1, __ATOMIC_RELAXED);
}
void note_packet_bad() {
    __atomic_add_fetch(&g_stats.artnet_bad_packets, 1, __ATOMIC_RELAXED);
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
