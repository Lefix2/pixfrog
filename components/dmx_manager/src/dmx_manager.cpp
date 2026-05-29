#include "dmx_manager.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace pixfrog::dmx {

namespace {

constexpr const char* TAG = "DMX";

// Two universe banks, each `kNumUniverses * kUniverseSize` bytes, in PSRAM.
uint8_t* g_uni_bank_a = nullptr;
uint8_t* g_uni_bank_b = nullptr;
std::atomic<uint8_t*> g_uni_front{nullptr};
uint8_t*              g_uni_back = nullptr;

// Per-channel pixel buffers in internal SRAM, double-buffered.
struct ChanBufs {
    uint8_t* a;
    uint8_t* b;
    std::atomic<uint8_t*> front;
    uint8_t* back;
};
ChanBufs g_chan_bufs[config::kNumChannels]{};

Stats               g_stats{};
std::atomic<bool>   g_sync_pending{false};

// Map a universe number to a slot in the bank. For v0 we use a simple
// flat allocation: universe N → slot (N % kNumUniverses). On boot, the
// config_store tells us which universes are routed to which channel; we
// keep this LUT for fast lookup.
//
// TODO(v1): replace with a hash map keyed by (net, subnet, universe).
uint16_t g_universe_to_slot[32768]{};
bool     g_universe_to_slot_valid = false;

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
    g_uni_back  = g_uni_bank_b;

    for (size_t i = 0; i < config::kNumChannels; ++i) {
        g_chan_bufs[i].a = static_cast<uint8_t*>(heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        g_chan_bufs[i].b = static_cast<uint8_t*>(heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!g_chan_bufs[i].a || !g_chan_bufs[i].b) {
            ESP_LOGE(TAG, "SRAM alloc for channel %u failed", static_cast<unsigned>(i));
            return false;
        }
        g_chan_bufs[i].front.store(g_chan_bufs[i].a, std::memory_order_release);
        g_chan_bufs[i].back = g_chan_bufs[i].b;
    }

    // Build the universe → slot LUT from current config.
    for (size_t i = 0; i < sizeof(g_universe_to_slot) / sizeof(g_universe_to_slot[0]); ++i) {
        g_universe_to_slot[i] = UINT16_MAX;
    }
    uint16_t slot = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc = config::get_channel(ch);
        // Estimate how many universes this channel spans (max 6 for 1024 RGB pixels).
        const size_t bytes_per_pixel = led::bytes_per_pixel(cc.protocol);
        const size_t total_bytes     = cc.pixel_count * bytes_per_pixel;
        const size_t universes_used  = (total_bytes + kUniverseSize - 1) / kUniverseSize;
        for (size_t u = 0; u < universes_used && slot < kNumUniverses; ++u) {
            g_universe_to_slot[cc.universe_start + u] = slot++;
        }
    }
    g_universe_to_slot_valid = true;
    ESP_LOGI(TAG, "init OK, allocated %u universe slots", slot);
    return true;
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

void note_packet_rx()  { __atomic_add_fetch(&g_stats.artnet_packets_rx, 1, __ATOMIC_RELAXED); }
void note_packet_bad() { __atomic_add_fetch(&g_stats.artnet_bad_packets, 1, __ATOMIC_RELAXED); }
void note_sync()       { g_sync_pending.store(true, std::memory_order_release); }

void swap_universes() {
    uint8_t* new_front = g_uni_back;
    g_uni_back = g_uni_front.exchange(new_front, std::memory_order_acq_rel);
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
    uint8_t* new_front = g_chan_bufs[ch].back;
    g_chan_bufs[ch].back = g_chan_bufs[ch].front.exchange(new_front, std::memory_order_acq_rel);
}

Stats get_stats() {
    Stats s = g_stats;
    return s;
}

void set_current_fps(uint32_t fps) { g_stats.current_fps = fps; }
void note_frame_emitted()          { __atomic_add_fetch(&g_stats.frames_emitted, 1, __ATOMIC_RELAXED); }
void note_dma_underrun()           { __atomic_add_fetch(&g_stats.dma_underruns, 1, __ATOMIC_RELAXED); }

}  // namespace pixfrog::dmx
