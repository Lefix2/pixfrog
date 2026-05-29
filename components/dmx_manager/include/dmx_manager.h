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

constexpr size_t kUniverseSize     = 512;  // bytes per DMX universe
constexpr size_t kNumUniverses     = 48;   // 8 channels × 6 max
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

// ── Configuration change propagation (item 7) ──────────────────────────────
// The UI commits config changes from `ui_task` (core 0) but the actual
// runtime state lives in render_task (core 1). To bridge them safely we
// use a FreeRTOS event group:
//   bit n (0..7)   — channel n config changed, remap LUT
//   bit 8          — global config changed (ArtNet net/subnet, network)
//
// UI calls `mark_channel_dirty(n)` or `mark_global_dirty()` after each
// successful NVS commit. render_task calls `handle_pending_remaps()` at
// the start of every frame; that function reads the bits, rebuilds the
// universe→channel LUT if any channel bit is set, and clears the bits.
// Bit 8 currently triggers a global LUT rebuild as well (cheap).
constexpr uint32_t kRemapAllChannelsMask = 0xFFu;
constexpr uint32_t kRemapGlobalBit       = 1u << 8;

void mark_channel_dirty(size_t channel_index);
void mark_global_dirty();
void handle_pending_remaps();

// Signal that an ArtSync was received (forces frame emission ASAP).
void note_sync();

// Block until either `period_ticks` elapse or an ArtSync arrives,
// whichever happens first. Returns true if a sync interrupted the wait,
// false if the timeout elapsed normally. Used by render_task to pace
// itself at refresh_rate_hz while still reacting to ArtSync within a
// few hundred microseconds.
bool wait_for_sync_or_period(uint32_t period_ticks);

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

// Copy DMX bytes for channel `ch` from the front universe bank into
// `pixel_back_buffer(ch)`. Spans multiple universes if a channel's pixel
// data straddles a universe boundary. Applies `dmx_start` as the per-channel
// offset into the FIRST universe (1-based per Art-Net convention).
// Returns true if all required universes were mapped; false otherwise (the
// remainder of the buffer is zero-filled to keep the strip in a defined state).
bool decode_pixels_for_channel(size_t ch);

// ────────────────────────────────────────────────────────────────────────────
// Capacity validation (per refresh rate)
// ────────────────────────────────────────────────────────────────────────────

// Returns true if channel `ch`'s configuration fits within one frame's
// emission budget. Updated by validate_capacity() (called at init and
// after every remap). Defaults to true.
bool is_channel_capacity_ok(size_t ch);

// Recompute per-channel capacity flags from current config + refresh rate.
// Logs warnings for over-capacity channels.
void validate_capacity();

// ────────────────────────────────────────────────────────────────────────────
// Telemetry
// ────────────────────────────────────────────────────────────────────────────

Stats get_stats();
void set_current_fps(uint32_t fps);
void note_frame_emitted();
void note_dma_underrun();

}  // namespace pixfrog::dmx
