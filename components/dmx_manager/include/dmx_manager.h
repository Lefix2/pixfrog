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

constexpr size_t kUniverseSize = 512;  // bytes per DMX universe
// 8 channels × 8 universes: a 1024-pixel RGBW channel spans 4096 B = 8
// universes, so anything less silently leaves the last channels unmapped.
constexpr size_t kNumUniverses = 64;
// Highest routable universe: the Art-Net Port-Address is 15-bit. sACN carries
// 1..63999 on the wire and FSEQ sparse ranges a 32-bit channel offset, so both
// must filter on universe_routable() before handing a number to this module.
constexpr uint16_t kMaxUniverseNumber = 0x7FFF;

inline bool universe_routable(uint32_t universe_number) {
    return universe_number <= kMaxUniverseNumber;
}

constexpr size_t kMaxPixelsPerChan = 1024;
constexpr size_t kMaxBytesPerChan  = kMaxPixelsPerChan * 4;  // RGBW worst case

// 2-source merge: how long a tracked sender may stay silent before it is
// dropped from the merge (per the receiver's protocol spec).
constexpr int64_t kArtnetMergeTimeoutUs = 10'000'000;  // Art-Net: ~10 s
constexpr int64_t kSacnMergeTimeoutUs   = 2'500'000;   // E1.31 §6.7.1 data loss

// Live telemetry counters; updated by render_task & receiver tasks with relaxed atomics.
struct Stats {
    uint64_t frames_emitted;
    uint64_t artnet_packets_rx;   // ArtDmx routed to a mapped universe
    uint64_t artnet_bad_packets;  // malformed (ArtNet or sACN)
    uint64_t artnet_ctrl_rx;      // ArtAddress/IpProg/Nzs/Trigger/Command/TimeCode
    uint64_t sacn_packets_rx;     // sACN data packets routed to a mapped universe
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

// ── 2-source merge (HTP/LTP) ────────────────────────────────────────────────
// Route one network frame into the universe pool, tracking up to two
// concurrent senders keyed by a nonzero `source_id` (IPv4 for ArtDmx, CID
// hash for sACN). With two live sources the output is merged per
// GlobalConfig::merge_mode; a source silent past `timeout_us` is dropped.
// Returns false if the universe is unmapped or the frame came from a third
// concurrent source (callers must not count rx/activity then).
bool write_universe_from_source(uint16_t universe_number, const uint8_t* data, size_t len,
                                uint32_t source_id, int64_t timeout_us);

// Forget one tracked source (sACN stream_terminated) or every source on a
// universe (sACN priority takeover) / the whole node (ArtAddress
// AcCancelMerge) — the next sender becomes exclusive.
void merge_drop_source(uint16_t universe_number, uint32_t source_id);
void merge_reset_universe(uint16_t universe_number);
void merge_cancel_all();

// True if any of the channel's universes currently has two live sources.
// Reported in ArtPollReply GoodOutputA bit 3 and chstat.
bool is_channel_merging(size_t channel_index);

// Note one ArtDmx packet was received. Used for stats only.
void note_packet_rx();
void note_packet_bad();
void note_ctrl_rx();  // ArtNet control packet (Address, IpProg, Nzs, ...)
void note_sacn_rx();  // sACN data packet routed to a mapped universe

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

// True if the channel is currently held in signal-loss failsafe (was active
// once, then silent past GlobalConfig::failsafe_timeout_s, mode != hold).
bool is_channel_failsafe(size_t channel_index);

// sACN stream_terminated (E1.31 §6.7.1): the source announced its end —
// expire the owning channel's activity immediately instead of waiting for
// the failsafe timeout. No-op for unmapped or never-active channels.
void note_universe_terminated(uint16_t universe_number);

// ── Configuration change propagation ───────────────────────────────────────
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

// ── Auto-patch ──────────────────────────────────────────────────────────────
// Re-address every channel contiguously from a flat 15-bit `base` universe:
// each channel is placed universe-aligned (dmx_start reset to 1) right after
// the previous one, advancing by its universe span (pixel_count × bytes/px).
// A disabled / 0-pixel channel consumes no universes. Persists each channel to
// NVS and marks it dirty so the LUT rebuilds next frame. Returns false if any
// NVS write failed (cache still updated); `*next_free` (if non-null) gets the
// first universe past the last channel. Console/web/ui context only.
bool auto_patch_universes(uint16_t base, uint16_t* next_free = nullptr);

// Test injection (control_console): write `len` bytes at byte `offset` into
// the universe's slot in BOTH banks. Writing both sides makes the data
// persistent across the per-frame bank swap — same "stale data sticks"
// convention as swap_universes(). Also refreshes the owning channel's
// activity timestamp. Returns false if the universe is unmapped or
// offset+len exceeds kUniverseSize.
bool inject_universe(uint16_t universe_number, size_t offset, const uint8_t* data, size_t len);

// Frame injection (FSEQ playback): a frame spanning several universes must be
// published by ONE bank swap, never half old / half new. begin() takes the
// swap lock, each universe() write goes to the back bank (seeded from the
// front, marked dirty), end() releases the lock — the render task's next
// swap_universes() then presents the whole frame at once. universe() returns
// false outside a begin()/end() pair, for an unmapped universe, or when
// offset+len exceeds kUniverseSize.
void inject_frame_begin();
bool inject_frame_universe(uint16_t universe_number, size_t offset, const uint8_t* data,
                           size_t len);
void inject_frame_end();

// ── Pixel-count preview (UI: ChPixels edit screen) ─────────────────────────
// While active, decode_pixels_for_channel(ch) fills the channel's pixel
// buffer with a coloured ruler (1..N-1 green, decades yellow, centades pink,
// N white) instead of decoding universes, and the output backend overrides
// the channel's pixel_count/brightness/grouping so the pattern maps 1:1 to
// physical LEDs. Set from ui_task, read from render_task (single atomic).
// A shrink emits one extra black-tail frame so dropped LEDs (which latch their
// last value) are turned off.
void set_pixel_preview(size_t channel_index, uint16_t pixel_count);
// While a gap is being edited, the ruler uses these runs instead of the stored
// ones so the dead LED follows the encoder before anything is committed.
// Cleared by clear_pixel_preview(). `n` ≤ led::kMaxPixelGaps, any order.
void set_preview_gaps(const led::PixelGap* gaps, size_t n);
void clear_pixel_preview();
// Returns the previewed channel (-1 if inactive) and its count.
int pixel_preview_channel();
uint16_t pixel_preview_count();
// Physical LED count the output stage must emit for the current preview frame
// (the lit count, or larger when a shrink owes a one-frame black tail).
uint16_t preview_emit_count();

// ── Channel identify (commissioning) ───────────────────────────────────────
// Blinks the channel full white at 2 Hz for `seconds` so the physical strip
// can be matched to its config slot. Auto-expires; any new call retargets.
void identify_start(size_t channel_index, uint16_t seconds = 10);
void identify_stop();
int identify_channel();  // -1 = inactive

// ── Standalone scenes (manual override), one per output ────────────────────
// Every output plays its own scene or the live input, so several scenes run
// at once on disjoint groups ("zones"). Starting a scene claims the outputs
// of its channel mask (∩ `outputs`) and leaves the others alone; a later
// start on an overlapping group takes those outputs over. While a scene
// plays on an output, network traffic for it is ignored until stopped
// (manual-stop policy). Changes crossfade over scene_fade_ms().
// Set from ui/console/web/artnet/render tasks (per-output atomics).
constexpr uint8_t kAllOutputs  = 0xFF;
constexpr int32_t kDefaultFade = -1;    // the configured / desk-overridden fade
void scene_start(uint8_t scene_index);  // on the scene's own mask
void scene_start_on(uint8_t scene_index, uint8_t outputs, int32_t fade_ms = kDefaultFade);
void scene_stop();  // every output back to live
void scene_stop_on(uint8_t outputs, int32_t fade_ms = kDefaultFade);
void scene_stop_scene(uint8_t scene_index);  // only the outputs playing it
// Call after config::delete_scene / move_scene: playing outputs follow their
// scene to its new position, or go back to live when it was deleted.
void scene_list_edited(config::SceneEdit op, size_t a, size_t b = 0);
int scene_on_output(size_t ch);        // -1 = live
uint8_t scene_outputs(uint8_t index);  // outputs playing scene `index`
int active_scene();                    // lowest output's scene, -1 = none (summary)
uint32_t scene_fade_ms();              // effective: desk Fade channel, else config

// ── Show control: grand master, blackout, strobe ────────────────────────────
// Local values (web / TFT / UART / ArtTrigger) combine with the control
// universe's: masters multiply, blackouts OR, the faster strobe wins. Runtime
// only — a reboot comes back at full, lit, steady. Identify and the pixel
// ruler bypass all three (commissioning must stay visible).
constexpr uint16_t kMasterFull = 65535;
void master_set(uint8_t outputs, uint16_t level);
uint16_t master_local(size_t ch);
uint16_t master_effective(size_t ch);
void blackout_set(uint8_t outputs, bool on);
void blackout_toggle(uint8_t outputs = kAllOutputs);  // dark if any is lit
uint8_t blackout_local();                             // outputs
uint8_t blackout_effective();                         // outputs
void strobe_set(uint8_t outputs, uint8_t hz10);       // tenths of Hz, 0 = off, ≤ 250
uint8_t strobe_local(size_t ch);
uint8_t strobe_effective(size_t ch);

// ── DMX control universe ────────────────────────────────────────────────────
// Evaluates config::get_control() against the front universe bank. Render
// task, once per frame right after swap_universes(). Levels (master, blackout,
// strobe, overrides) follow the desk continuously; triggers (scene, FSEQ) act
// when their band changes — the first frame after the universe appears only
// acts on non-zero bands, so an idle desk does not stop a local scene.
// When the universe goes silent (kControlTimeoutUs), master/blackout/strobe
// return to neutral (never left dark); scenes and overrides stay.
constexpr int64_t kControlTimeoutUs = 3'000'000;
void update_show_control();
bool control_live();
int control_universe();  // -1 = disabled (sACN joins it when enabled)
// FSEQ requests from the desk, consumed by fseq_player (render task must not
// block on SD): kFseqNoRequest, kFseqStopRequest, or a 0-based file index.
constexpr int16_t kFseqNoRequest   = -2;
constexpr int16_t kFseqStopRequest = -1;
int16_t take_fseq_request();
// Diagnostics: pool slot of the control universe (-1 = unmapped) and ms since
// its last packet (-1 = never).
int control_pool_slot();
int64_t control_last_rx_ms_ago();
// Effect a desk's Effect channel value selects (-1 = the scene's own).
int effect_for_dmx_value(uint8_t v);

// ── FSEQ playback ────────────────────────────────────────────────────────────
// fseq_player calls fseq_set_active(true) while a show is running.
// decode_pixels_for_channel() skips the failsafe check while FSEQ is active
// so a buffering pause doesn't blackout the output.
void fseq_set_active(bool active);
bool fseq_is_active();

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

// Live preview for the web dashboard: channel `ch`'s front buffer shrunk to at
// most `max_samples` RGB triplets in `rgb` (see logic::downsample_rgb). Any
// task; a frame swapped mid-read only tears the preview. 0 for an Off channel.
size_t output_preview(size_t ch, uint8_t* rgb, size_t max_samples);

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

// Returns true if channel `ch`'s requested pixel count fits within one frame's
// emission budget; false means it is emitted truncated (effective_channel).
// Updated by validate_capacity() (called at init and after every remap).
bool is_channel_capacity_ok(size_t ch);

// Recompute per-channel capacity flags from current config + refresh rate.
// Logs warnings for over-capacity channels.
void validate_capacity();

// Largest pixel_count channel `ch` may use at the current refresh rate without
// outrunning the bus or the DMA buffer (see logic::max_pixels_for). The UI uses
// this as the upper bound of the pixel-count editor.
uint16_t channel_max_pixels(size_t ch);

// The channel as emitted: its stored config with pixel_count limited to
// channel_max_pixels(). Every consumer that sizes or fills the output (decode,
// effects, encoder, pacing) goes through this; the stored count stays as set.
config::ChannelConfig effective_channel(size_t ch);

// Pixels on the wire for `cc`'s (live) pixel_count: live + dead (gaps).
uint32_t physical_pixels(const config::ChannelConfig& cc);
// Dead-pixel runs in use on channel `ch` (0 for an Off channel).
size_t channel_gap_count(size_t ch);

// Physical emission time (µs) of the current frame: the longest configured
// channel's wire time (all channels emit in parallel on the shared bus). The
// render loop never paces faster than this, so a channel whose frame outlasts
// the configured period (a long strip at a high refresh) emits intact at its
// own rate instead of being over-submitted.
uint64_t frame_emit_us();

// ────────────────────────────────────────────────────────────────────────────
// Telemetry
// ────────────────────────────────────────────────────────────────────────────

Stats get_stats();
void set_current_fps(uint32_t fps);
void note_frame_emitted();
void note_dma_underrun();

}  // namespace pixfrog::dmx
