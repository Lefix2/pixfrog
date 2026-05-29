// dmx_manager — universe pool, channel mapping, double-buffered pixel buffers.
//
// Holds the back/front universe banks written by the artnet receiver, and the
// per-channel pixel buffers consumed by the LCD_CAM driver.
//
// All buffers are allocated once at init() and never resized at runtime.

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"

namespace pixfrog::dmx {

constexpr size_t kUniverseSize     = 512;          // bytes per DMX universe
constexpr size_t kNumUniverses     = 48;           // 8 channels × 6 max
constexpr size_t kMaxPixelsPerChan = 1024;
constexpr size_t kMaxBytesPerChan  = kMaxPixelsPerChan * 4;  // RGBW worst case

// Live telemetry counters; updated by render_task & artnet_task with relaxed atomics.
struct Stats {
    uint64_t frames_emitted;
    uint64_t artnet_packets_rx;
    uint64_t artnet_bad_packets;
    uint32_t dma_underruns;
    uint32_t current_fps;
};

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

// Allocates the universe pool (PSRAM) and the pixel buffers (SRAM).
// Returns false on allocation failure (out of memory).
bool init();

// ────────────────────────────────────────────────────────────────────────────
// ArtNet ingest side (artnet_task on core 0)
// ────────────────────────────────────────────────────────────────────────────

// Resolve a universe number (net+subnet+universe combined into 0..32767)
// into a pool slot. Returns nullptr if the universe is not configured for any
// channel. The returned pointer is valid until the next swap_universes().
uint8_t* universe_back_buffer_for(uint16_t universe_number);

// Note one ArtDmx packet was received. Used for stats only.
void note_packet_rx();
void note_packet_bad();

// Note that a DMX update arrived for `channel_index` (derived from the
// universe number by the caller via channel_for_universe).
// Refreshes the per-channel "last activity" timestamp used by HOME render.
void note_channel_activity(size_t channel_index);

// Returns the channel index this universe was assigned to at init() time,
// or -1 if the universe number is not mapped to any channel.
int channel_for_universe(uint16_t universe_number);

// True if `channel_index` received at least one ArtDmx packet within the
// last second. Used by ui::set_channel_active() at HOME refresh time.
bool is_channel_active(size_t channel_index);

// Signal that an ArtSync was received (forces frame emission ASAP).
void note_sync();

// ────────────────────────────────────────────────────────────────────────────
// Render side (render_task on core 1)
// ────────────────────────────────────────────────────────────────────────────

// Swap front/back universe banks atomically. Call once per frame.
void swap_universes();

// Read-only access to the front universe (cf. atomic swap). May be nullptr
// between init and the first swap.
const uint8_t* universe_front_buffer_for(uint16_t universe_number);

// Get the writable back pixel buffer for channel `ch`. Returns a pointer
// into pre-allocated SRAM, never null.
uint8_t* pixel_back_buffer(size_t ch);

// Swap front/back pixel buffers for channel `ch`. Atomic pointer swap.
void swap_pixels(size_t ch);

// Read-only access to the front pixel buffer for channel `ch`.
const uint8_t* pixel_front_buffer(size_t ch);

// ────────────────────────────────────────────────────────────────────────────
// Telemetry
// ────────────────────────────────────────────────────────────────────────────

Stats get_stats();
void  set_current_fps(uint32_t fps);
void  note_frame_emitted();
void  note_dma_underrun();

}  // namespace pixfrog::dmx
