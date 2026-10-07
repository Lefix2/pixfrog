#include "dmx_manager.h"

#include <atomic>
#include <cstdint>
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

// dmx_logic.h is deliberately standalone (the host suite includes it alone),
// so it carries its own copies of these. Pin them together here.
static_assert(logic::kUniverseSize == kUniverseSize);
static_assert(logic::kMaxUniverseNumber == kMaxUniverseNumber);

// Two universe banks, each `kNumUniverses * kUniverseSize` bytes, in PSRAM.
// Both pointers are set once in init() and never move afterwards, so "the back
// bank" is just the one `g_uni_front` is not currently on. Deriving it that way
// (rather than keeping a second `g_uni_back` pointer that the render task
// rewrites on every swap) means there is no non-atomic shared pointer for the
// receiver task to catch mid-swap.
uint8_t* g_uni_bank_a = nullptr;
uint8_t* g_uni_bank_b = nullptr;
std::atomic<uint8_t*> g_uni_front{ nullptr };

// Serialises the front/back flip in swap_universes() (render task) against a
// receiver-task write. Without it the receiver can resolve the back bank, get
// pre-empted by a swap, and finish its write into the bank the decode is now
// reading — a torn frame, and a dirty bit the swap has already consumed. Held
// across one universe-sized memcpy by a writer, so the render task's worst-case
// wait here is microseconds.
SemaphoreHandle_t g_uni_swap_mux = nullptr;

// One bit per pool slot, set once the receiver has written that slot into the
// CURRENT back bank and cleared by swap_universes(). It does two jobs:
//   - `mask == 0` means nothing arrived since the last swap, so swap_universes()
//     must NOT swap. Swapping unconditionally re-presents the bank that is one
//     source frame behind, so any source slower than the refresh rate alternates
//     between its last two frames instead of holding the newest — a 10 Hz source
//     against a 61 Hz refresh flickers at ~30 Hz.
//   - the first write to a slot after a swap seeds that slot from the front bank,
//     so a packet shorter than the universe cannot leave a tail from two source
//     frames ago.
// Both run with g_uni_swap_mux held, so a seed cannot race the swap.
// 32-bit words: read-modify-write atomics on the ESP32-P4 must be 32-bit.
constexpr size_t kDirtyWords = (kNumUniverses + 31) / 32;
std::atomic<uint32_t> g_uni_dirty[kDirtyWords]{};

bool any_dirty() {
    for (const auto& w : g_uni_dirty)
        if (w.load(std::memory_order_acquire)) return true;
    return false;
}

void clear_dirty() {
    for (auto& w : g_uni_dirty)
        w.store(0, std::memory_order_relaxed);
}

// One bit per pool slot: written before the last swap, so the back bank holds
// its value from one source frame earlier. A slot nobody writes again would
// come back with that older value at the next swap — two senders at different
// rates, or a universe sent on change only, flickered between their last two
// frames. swap_universes() brings such a slot level before presenting the
// bank: one copy per slot after its last write, none while every universe
// arrives every frame. Only touched with g_uni_swap_mux held.
uint32_t g_uni_behind[kDirtyWords]{};

// Call with g_uni_swap_mux held, before the flip.
void level_back_bank(uint8_t* back, const uint8_t* front) {
    for (size_t w = 0; w < kDirtyWords; ++w) {
        const uint32_t dirty = g_uni_dirty[w].load(std::memory_order_acquire);
        uint32_t stale       = g_uni_behind[w] & ~dirty;
        g_uni_behind[w]      = dirty;
        for (; stale; stale &= stale - 1) {
            const size_t base = (w * 32 + static_cast<size_t>(__builtin_ctz(stale))) *
                                kUniverseSize;
            memcpy(back + base, front + base, kUniverseSize);
        }
    }
}

// Per-channel pixel buffers in internal SRAM, double-buffered.
struct ChanBufs {
    uint8_t* a;
    uint8_t* b;
    std::atomic<uint8_t*> front;
    uint8_t* back;
};
ChanBufs g_chan_bufs[config::kNumChannels]{};

Stats g_stats{};
// Sync mode (see dmx_manager.h). 32-bit: exchange() is a read-modify-write,
// and sub-word RMW atomics clobber their neighbours on the P4.
std::atomic<uint32_t> g_sync_pending{ 0 };
std::atomic<uint32_t> g_sync_until_ms{ 0 };  // sync mode while now is before this
std::atomic<uint32_t> g_sync_force{ 0 };     // E1.31 Force_Synchronization: hold for good
std::atomic<uint32_t> g_sync_seen{ 0 };      // a sync or a sync hold ever arrived

// Pixel-count preview state: channel (high 16 bits) + count (low 16 bits)
// packed into one atomic so render_task always reads a consistent pair.
// 0xFFFF in the channel half = preview inactive.
constexpr uint32_t kPreviewOff = 0xFFFF0000u;
std::atomic<uint32_t> g_pixel_preview{ kPreviewOff };

// Pixel-count shrink erase: when the previewed count drops, the LEDs it just
// dropped hold their last value until overwritten. `g_preview_erase` is the
// extent the UI owes a black tail down to; the render task consumes it on the
// next decode so exactly one frame carries the erase. `g_preview_emit` is the
// physical LED count that frame emits (= max(count, erase)): decode writes it
// into `g_preview_emit_next`, and swapping the channel's pixels hands it to the
// output stage with the frame it belongs to (decode runs a frame ahead of the
// encode, on the other core).
std::atomic<uint32_t> g_preview_erase{ 0 };  // RMW: 32-bit (see g_local_blackout)
// Gap-edit override for the ruler (set from ui_task, read by render_task; a
// torn copy costs one oddly-marked preview frame).
led::PixelGap g_preview_gaps[led::kMaxPixelGaps]{};
std::atomic<bool> g_preview_gaps_on{ false };
std::atomic<uint16_t> g_preview_emit{ 0 };
std::atomic<uint16_t> g_preview_emit_next{ 0 };

// Scene per output (-1 = live input) and its crossfade: the source it fades
// from (-1 = live), when it started and how long it lasts. Written by any
// task, read by render_task; a torn set costs one oddly-blended frame.
std::atomic<int8_t> g_scene_out[config::kNumChannels];
std::atomic<int8_t> g_fade_from[config::kNumChannels];
std::atomic<uint32_t> g_fade_start_ms[config::kNumChannels];
std::atomic<uint16_t> g_fade_len_ms[config::kNumChannels];
// Crossfade source render target (render task only), internal SRAM.
uint8_t* g_scratch = nullptr;

// ── Scenes on groups ──
// Writers (any task, under g_play_mux): the plays (scene × group) and which
// play owns each fixture (-1 = none: the fixture shows its output's own
// source), plus a crossfade request per fixture whose owner changed. The
// render task copies all that once a frame (plays_frame_begin) and draws each
// play once along its group, then lays the slices over the outputs.
constexpr size_t kMaxPlays    = 16;
constexpr size_t kMaxStripPx  = 4096;  // a group's virtual strip
constexpr uint16_t kNoFadeReq = 0xFFFF;
struct Play {
    int8_t scene = -1;  // -1 = slot free
    int8_t group = -1;
};
SemaphoreHandle_t g_play_mux = nullptr;
Play g_plays[kMaxPlays];
int8_t g_owner[config::kNumChannels][config::kMaxFixtures];
uint16_t g_fade_req[config::kNumChannels][config::kMaxFixtures];  // ms, kNoFadeReq = none
// Render task only: this frame's copy, the spans, the strips, the fades.
Play g_plays_r[kMaxPlays];
int8_t g_owner_r[config::kNumChannels][config::kMaxFixtures];
uint16_t g_fade_req_r[config::kNumChannels][config::kMaxFixtures];
config::FixtureGroup g_group_r[kMaxPlays];
logic::Span g_spans_r[config::kNumChannels][config::kMaxFixtures];
uint8_t* g_play_strip[kMaxPlays]{};  // PSRAM, kMaxStripPx RGB each
uint32_t g_play_offs[kMaxPlays][config::kMaxGroupMembers];
uint16_t g_play_lens[kMaxPlays][config::kMaxGroupMembers];
bool g_play_ok[kMaxPlays];
uint8_t* g_fix_snap[config::kNumChannels]{};  // PSRAM: what a fading fixture showed
uint32_t g_fix_fade_start[config::kNumChannels][config::kMaxFixtures];
uint16_t g_fix_fade_len[config::kNumChannels][config::kMaxFixtures];

// ── Effect clocks and look transitions (render task only) ──
// Where each look is, kept from frame to frame so a speed change bends the
// motion instead of jumping it (logic::FxClock): four an output — the scene
// it plays and the one it fades from, each maybe between two looks of the
// desk — two a group play, and two a fixture under fixture control (PSRAM,
// kMaxFixtures an output). The desk's Effect / Generator pick on an output or
// a group, and a fixture's Effect channel, crossfade between looks
// (logic::LookFade): the outputs' and groups' over the scene fade time, a
// fixture's over its FX fade channel.
logic::FxClock g_out_clock[config::kNumChannels][4];
logic::FxClock g_play_clock[kMaxPlays][2];
logic::FixtureFx* g_fix_fx[config::kNumChannels]{};
logic::LookFade g_out_look[config::kNumChannels];
logic::LookFade g_play_look[kMaxPlays];
Play g_play_was[kMaxPlays];         // what each play slot drew last frame: a new play starts afresh
uint8_t* g_fx_scratch   = nullptr;  // PSRAM, kMaxBytesPerChan: the look a fade leaves
uint8_t* g_play_scratch = nullptr;  // PSRAM, kMaxStripPx RGB: the same for a group
// The frame's time, taken once at the swap: every output ticks its clocks at
// the same instant, so outputs whose speed a desk rides together stay in step.
// A channel decoded again before the next swap reads the timer.
uint64_t g_frame_ms  = 0;
uint32_t g_frame_seq = 0;
uint32_t g_ch_seq[config::kNumChannels]{};

// Local show values (web/TFT/UART/ArtTrigger).
std::atomic<uint16_t> g_local_master[config::kNumChannels];
// Read-modify-write atomics (exchange, fetch_*) must be 32-bit on the P4: a
// sub-word RMW is an LR/SC on the whole word, and a concurrent store from the
// other core to a neighbour in that word was lost (the control universe slot
// kept reverting next to a 16-bit exchange). tools/lint_atomics.py enforces it.
std::atomic<uint32_t> g_local_blackout{ 0 };
std::atomic<uint8_t> g_local_strobe[config::kNumChannels];
// Control-universe values, published by update_show_control (render task).
std::atomic<uint16_t> g_dmx_master[config::kNumChannels];
std::atomic<uint8_t> g_dmx_blackout{ 0 };
std::atomic<uint8_t> g_dmx_strobe[config::kNumChannels];
std::atomic<int32_t> g_dmx_fade_ms{ -1 };
// Render-task-only control state.
logic::EffectOverride g_ovr[config::kNumChannels];
// The same for group-targeted control slots (render task only): overrides for
// the scenes playing on each group, and a master / blackout over its bars.
logic::EffectOverride g_govr[config::kMaxGroups];
uint16_t g_gmaster[config::kMaxGroups];
uint32_t g_gblackout = 0;
uint8_t g_ctrl_prev_band[config::kMaxControlSlots];
int16_t g_ctrl_prev_fseq = -1;
bool g_ctrl_was_live     = false;
// Pool slot of the control universe (kNoSlot = disabled/unmapped) and the
// last time a packet reached it.
uint16_t g_ctrl_slot = 0xFFFF;
std::atomic<int64_t> g_ctrl_last_us{ 0 };
std::atomic<int32_t> g_fseq_request{ kFseqNoRequest };  // 32-bit: see g_local_blackout
constexpr uint8_t kNoChannel = 0xFF;                    // a pool slot that feeds no output

// FSEQ playback active flag.
std::atomic<bool> g_fseq_active{ false };

// Identify blink: channel in the high byte (0xFF = off), expiry in ms since
// boot in the low 24 bits won't fit — use two relaxed atomics; tearing across
// them costs at most one oddly-timed frame.
// Identify sequence: the outputs left to blink (bit n = output n, 32-bit for
// the P4 RMW rule), its start and the blinks per output.
std::atomic<uint32_t> g_identify_mask{ 0 };
std::atomic<uint32_t> g_identify_t0_ms{ 0 };
std::atomic<uint32_t> g_identify_blinks{ kIdentifyBlinks };

// Universe → slot lookup, indexed by the flat 15-bit Art-Net Port-Address.
// Sized for the whole addressable range so routing is a single load; every
// read goes through slot_for_universe(), which rejects the out-of-range
// numbers sACN and FSEQ can produce.
uint16_t g_universe_to_slot[logic::kMaxUniverseNumber + 1]{};
bool g_universe_to_slot_valid = false;

// Reverse mapping slot → the channels it feeds (bit n = channel n; two with
// compact patching sharing a universe), populated alongside g_universe_to_slot.
uint8_t g_slot_chans[kNumUniverses]{};
uint16_t g_slots_used = 0;

// 2-source merge: per-slot source tracking + per-source staging frames
// (2 × kUniverseSize per slot, PSRAM). Receiver tasks (artnet + sacn, same
// priority, core 0) may interleave here at tick boundaries; like the bank
// writes, a torn merge costs at most one oddly-blended frame.
logic::MergeState g_merge[kNumUniverses]{};
uint8_t* g_merge_staging = nullptr;

// Per-channel last-activity timestamp (µs). 0 = never seen; kTerminatedUs =
// the source announced its end (sACN stream_terminated).
constexpr int64_t kTerminatedUs = INT64_MIN / 2;
// Per-channel last-activity timestamp (µs). 0 = never seen. Written by the
// receivers (core 0), read by render_task (core 1): a plain int64 is two
// 32-bit accesses on the P4, so a reader could see mixed halves at a 2³² µs
// rollover — one spurious failsafe frame. Load/store only (no RMW), so the
// P4's word-sized RMW rule does not apply; IDF makes them indivisible.
std::atomic<int64_t> g_last_activity_us[config::kNumChannels]{};
// "Active" if last_activity within this window:
constexpr int64_t kActivityWindowUs = 1'000'000;  // 1 second

// Per-channel capacity flag. True if the channel's encoded frame
// fits within one refresh period. Defaults to true (assumed OK until
// proven otherwise by validate_capacity).
bool g_channel_capacity_ok[config::kNumChannels]{};

// Event group for config change propagation.
EventGroupHandle_t g_remap_eg = nullptr;

// Binary semaphore for the ArtSync → render_task fast path.
SemaphoreHandle_t g_sync_sem = nullptr;

// Rebuild the universe → slot LUT (+ reverse slot → channel) from the
// current contents of config_store. Called from init() and from
// handle_pending_remaps() when the UI signals a config change.
void rebuild_universe_lut() {
    // Static: 8 channel configs (fixtures included) are ~1.7 kB, too much for
    // app_main's stack at boot. init() and the render task never overlap.
    static config::ChannelConfig chans[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        chans[ch] = config::get_channel(ch);

    size_t unmapped = 0;
    g_slots_used    = logic::build_universe_map(chans, config::kNumChannels, g_universe_to_slot,
                                                g_slot_chans, kNumUniverses, &unmapped,
                                                &config::get_profiles());
    // Silent truncation used to look exactly like a patching mistake on the
    // console side, so say it out loud.
    if (unmapped) {
        ESP_LOGE(TAG,
                 "%u universe(s) could not be mapped (pool %u slots, max universe %u) — "
                 "those channels will receive nothing",
                 static_cast<unsigned>(unmapped), static_cast<unsigned>(kNumUniverses),
                 static_cast<unsigned>(logic::kMaxUniverseNumber));
    }
    // The control universe: its own slot unless an output already patched
    // it (then both read the same data — the UI warns about the overlap).
    g_ctrl_slot      = logic::kNoSlot;
    const auto& ctrl = config::get_control();
    if (ctrl.enabled && logic::universe_routable(ctrl.universe)) {
        const uint16_t have = g_universe_to_slot[ctrl.universe];
        if (have != logic::kNoSlot && have < g_slots_used) {
            g_ctrl_slot = have;
        } else if (g_slots_used < kNumUniverses) {
            g_ctrl_slot                       = g_slots_used;
            g_universe_to_slot[ctrl.universe] = g_slots_used;
            g_slot_chans[g_slots_used]        = 0;  // feeds no output
            ++g_slots_used;
        } else {
            ESP_LOGE(TAG, "no pool slot left for the control universe %u",
                     static_cast<unsigned>(ctrl.universe));
        }
    }
    if (ctrl.enabled)
        ESP_LOGI(TAG, "control universe %u -> pool slot %d", static_cast<unsigned>(ctrl.universe),
                 g_ctrl_slot == logic::kNoSlot ? -1 : static_cast<int>(g_ctrl_slot));
    // Slots may now mean different universes — tracked sources are stale.
    std::memset(g_merge, 0, sizeof(g_merge));
}

// Every universe number reaching the pool is caller-supplied: Art-Net masks it
// to 15 bits, but sACN carries 1..63999 and FSEQ sparse ranges a 32-bit channel
// offset. Bounds-check once, here, so no caller can index past the table.
uint16_t slot_for_universe(uint16_t universe_number) {
    if (!g_universe_to_slot_valid || !logic::universe_routable(universe_number))
        return logic::kNoSlot;
    const uint16_t slot = g_universe_to_slot[universe_number];
    return slot < g_slots_used ? slot : logic::kNoSlot;
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
    clear_dirty();

    g_uni_swap_mux = xSemaphoreCreateMutex();
    if (!g_uni_swap_mux) {
        ESP_LOGE(TAG, "universe swap mutex alloc failed");
        return false;
    }

    g_merge_staging = static_cast<uint8_t*>(
        heap_caps_calloc(1, kNumUniverses * 2 * kUniverseSize, MALLOC_CAP_SPIRAM));
    if (!g_merge_staging) {
        ESP_LOGE(TAG, "PSRAM alloc for merge staging failed");
        return false;
    }

    g_scratch = static_cast<uint8_t*>(
        heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!g_scratch) {
        ESP_LOGE(TAG, "SRAM alloc for the crossfade buffer failed");
        return false;
    }
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        g_scene_out[i].store(-1, std::memory_order_relaxed);
        g_fade_from[i].store(-1, std::memory_order_relaxed);
        g_fade_len_ms[i].store(0, std::memory_order_relaxed);
        g_local_master[i].store(kMasterFull, std::memory_order_relaxed);
        g_local_strobe[i].store(0, std::memory_order_relaxed);
        g_dmx_master[i].store(kMasterFull, std::memory_order_relaxed);
        g_dmx_strobe[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        // In PSRAM: a frame touches ~12 kB of them against the encoder's
        // ~0.5 MB of frame buffer, and the 64 kB they took in internal RAM is
        // what the output's DMA lists need, in one block, at every
        // frame-length change (a live resize failed for want of it).
        g_chan_bufs[i].a = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_SPIRAM));
        g_chan_bufs[i].b = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_SPIRAM));
        if (!g_chan_bufs[i].a || !g_chan_bufs[i].b) {
            ESP_LOGE(TAG, "PSRAM alloc for channel %u failed", static_cast<unsigned>(i));
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

    // Scenes on groups: a strip per play and a snapshot per output, in PSRAM
    // (allocated once: init() may run again).
    for (auto& m : g_gmaster)
        m = kMasterFull;
    if (!g_play_mux) g_play_mux = xSemaphoreCreateMutex();
    if (!g_play_mux) {
        ESP_LOGE(TAG, "group plays mutex alloc failed");
        return false;
    }
    for (auto& row : g_owner)
        for (auto& o : row)
            o = -1;
    for (auto& row : g_fade_req)
        for (auto& r : row)
            r = kNoFadeReq;
    for (size_t i = 0; i < kMaxPlays; ++i)
        if (!g_play_strip[i])
            g_play_strip[i] = static_cast<uint8_t*>(
                heap_caps_calloc(1, kMaxStripPx * 3, MALLOC_CAP_SPIRAM));
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if (!g_fix_snap[o])
            g_fix_snap[o] = static_cast<uint8_t*>(
                heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_SPIRAM));
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if (!g_fix_fx[o])
            g_fix_fx[o] = static_cast<logic::FixtureFx*>(heap_caps_calloc(
                config::kMaxFixtures, sizeof(logic::FixtureFx), MALLOC_CAP_SPIRAM));
    if (!g_fx_scratch)
        g_fx_scratch = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxBytesPerChan, MALLOC_CAP_SPIRAM));
    if (!g_play_scratch)
        g_play_scratch = static_cast<uint8_t*>(
            heap_caps_calloc(1, kMaxStripPx * 3, MALLOC_CAP_SPIRAM));
    bool all = g_play_mux != nullptr;
    for (auto* p : g_play_strip)
        all = all && p;
    for (auto* p : g_fix_snap)
        all = all && p;
    for (auto* p : g_fix_fx)
        all = all && p;
    all = all && g_fx_scratch && g_play_scratch;
    if (!all) {
        ESP_LOGE(TAG, "alloc for the group scenes failed");
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

bool auto_patch_universes(uint16_t base, uint16_t* next_free) {
    AutoPatch o;
    o.base = base;
    return auto_patch(o, next_free);
}

bool auto_patch(const AutoPatch& opt, uint16_t* next_free, size_t* universes) {
    // On the heap: ~1.7 kB of channel configs on the ui task's 4 kB stack (the
    // menu's Auto-patch) is asking for an overflow. A one-off config action.
    auto* chans = static_cast<config::ChannelConfig*>(
        heap_caps_malloc(sizeof(config::ChannelConfig) * config::kNumChannels, MALLOC_CAP_DEFAULT));
    if (!chans) return false;
    for (size_t i = 0; i < config::kNumChannels; ++i)
        chans[i] = config::get_channel(i);

    logic::AutoPatchOptions o;
    o.base     = opt.base;
    o.compact  = opt.compact;
    o.packing  = opt.packing;
    o.fix_base = opt.fix_base;
    uint16_t starts[config::kNumChannels], dmx[config::kNumChannels];
    size_t used   = 0;
    uint16_t next = logic::compute_auto_patch(o, chans, config::kNumChannels, starts, dmx, nullptr,
                                              &config::get_profiles(), &used);
    // The blocks are sequential from their bases: an end past the last
    // universe shows unmasked (the cursor itself wraps past 0x7FFF).
    const uint32_t end = static_cast<uint32_t>(opt.base) + used;

    bool all_persisted = true;
    // The DMX control universe, when used, is the third block: it opens the
    // universe after the pixels and the fixtures.
    auto ctl = config::get_control();
    if (ctl.enabled && end <= kMaxUniverseNumber) {
        ctl.universe = next++;
        ctl.address  = 1;
        ++used;
        all_persisted &= config::set_control(ctl);
        mark_global_dirty();
    }
    if (next_free) *next_free = next;
    if (universes) *universes = used;

    for (size_t i = 0; i < config::kNumChannels; ++i) {
        all_persisted &= config::set_channel(i, chans[i]);
        mark_channel_dirty(i);
    }
    heap_caps_free(chans);
    return all_persisted;
}

size_t channel_pixel_span(const config::ChannelConfig& cc) {
    return logic::pixel_universes_used(cc, &config::get_profiles());
}

size_t channel_fixture_span(const config::ChannelConfig& cc) {
    return logic::fixture_universes_used(cc, &config::get_profiles());
}

size_t channel_universe_span(const config::ChannelConfig& cc) {
    return logic::channel_universes_used(cc, &config::get_profiles());
}

bool fixtures_clear_of_pixels(size_t ch, config::ChannelConfig& cc) {
    return logic::fixtures_clear_of_pixels(
        cc, ch, config::kNumChannels,
        [](size_t i) -> const config::ChannelConfig& { return config::get_channel(i); },
        &config::get_profiles());
}

size_t fixture_patch(const config::ChannelConfig& cc, FixtureAddress* out, size_t cap) {
    if (!config::fixture_controlled(cc)) return 0;
    size_t n = 0;
    logic::for_each_fixture_patch(cc, config::get_profiles(), [&](const logic::FixturePatch& f) {
        if (n < cap)
            out[n] = FixtureAddress{ static_cast<uint16_t>(config::fix_universe(cc) + f.uni_off),
                                     static_cast<uint16_t>(f.slot + 1), f.footprint, f.profile };
        ++n;
    });
    return n < cap ? n : cap;
}

static_assert(kPatchControl == logic::kPatchControl && kMaxPatchClashes == logic::kMaxPatchClashes,
              "the patch owners are numbered the same on both sides");

size_t patch_clashes(PatchClash* out, size_t cap) {
    if (!out || cap == 0) return 0;
    // ~15 kB of ranges and the pairs: off the caller's stack (an HTTP handler,
    // the console).
    struct Work {
        logic::PatchRange ranges[logic::kMaxPatchRanges];
        logic::PatchClash pairs[logic::kMaxPatchClashes];
    };
    auto* w = static_cast<Work*>(heap_caps_malloc(sizeof(Work), MALLOC_CAP_SPIRAM));
    if (!w) return 0;
    const size_t nr = logic::collect_patch_ranges(
        [](size_t o) -> const config::ChannelConfig& { return config::get_channel(o); },
        config::kNumChannels, config::get_profiles(), config::get_control(), w->ranges,
        logic::kMaxPatchRanges);
    const size_t np = logic::find_patch_clashes(w->ranges, nr, w->pairs, logic::kMaxPatchClashes);
    const size_t n  = np < cap ? np : cap;
    for (size_t i = 0; i < n; ++i) {
        const logic::PatchClash& p = w->pairs[i];
        out[i] = PatchClash{ p.a, p.b, p.universe, static_cast<uint16_t>(p.slot + 1), p.channels };
    }
    heap_caps_free(w);
    return n;
}

uint8_t patch_clash_outputs(const PatchClash* c, size_t n) {
    uint8_t mask = 0;
    for (size_t i = 0; i < n; ++i)
        for (const uint8_t owner : { c[i].a, c[i].b })
            if (owner < kPatchControl) mask |= 1u << (owner % config::kNumChannels);
    return mask;
}

void set_pixel_preview(size_t channel_index, uint16_t pixel_count) {
    if (channel_index >= config::kNumChannels) return;
    const uint32_t prev = g_pixel_preview.load(std::memory_order_relaxed);
    // A shrink on the same channel owes a one-frame black tail down from the
    // previously shown count. Accumulate the largest extent so several fast
    // detents collapse into a single erase before the next decode flushes it.
    if ((prev >> 16) == channel_index) {
        const uint16_t prev_count = static_cast<uint16_t>(prev & 0xFFFFu);
        if (pixel_count < prev_count &&
            prev_count > g_preview_erase.load(std::memory_order_relaxed))
            g_preview_erase.store(prev_count, std::memory_order_relaxed);
    }
    g_pixel_preview.store((static_cast<uint32_t>(channel_index) << 16) | pixel_count,
                          std::memory_order_relaxed);
}

void set_preview_gaps(const led::PixelGap* gaps, size_t n) {
    led::PixelGap tmp[led::kMaxPixelGaps]{};
    for (size_t k = 0; k < n && k < led::kMaxPixelGaps; ++k)
        tmp[k] = gaps[k];
    led::normalize_gaps(tmp, led::kMaxPixelGaps);
    std::memcpy(g_preview_gaps, tmp, sizeof(tmp));
    g_preview_gaps_on.store(true, std::memory_order_release);
}

void clear_pixel_preview() {
    g_preview_gaps_on.store(false, std::memory_order_relaxed);
    g_pixel_preview.store(kPreviewOff, std::memory_order_relaxed);
    g_preview_erase.store(0, std::memory_order_relaxed);
}

int pixel_preview_channel() {
    const uint32_t v  = g_pixel_preview.load(std::memory_order_relaxed);
    const uint32_t ch = v >> 16;
    return ch == 0xFFFFu ? -1 : static_cast<int>(ch);
}

uint16_t pixel_preview_count() {
    return static_cast<uint16_t>(g_pixel_preview.load(std::memory_order_relaxed) & 0xFFFFu);
}

// Physical LED count for the frame currently being built (decode publishes it;
// the output stage reads it back to size the emission incl. any erase tail).
uint16_t preview_emit_count() {
    return g_preview_emit.load(std::memory_order_relaxed);
}

void identify_outputs(uint8_t outputs, uint8_t blinks) {
    const uint32_t mask = outputs & ((1u << config::kNumChannels) - 1u);
    g_identify_blinks.store(blinks ? blinks : 1, std::memory_order_relaxed);
    g_identify_t0_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000),
                           std::memory_order_relaxed);
    g_identify_mask.store(mask, std::memory_order_release);
}

void identify_start(size_t channel_index, uint8_t blinks) {
    if (channel_index >= config::kNumChannels) return;
    identify_outputs(static_cast<uint8_t>(1u << channel_index), blinks);
}

uint8_t identify_configured_outputs() {
    uint8_t mask = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        if (config::get_channel(ch).protocol != led::Protocol::Off) mask |= 1u << ch;
    return mask;
}

void identify_stop() {
    g_identify_mask.store(0, std::memory_order_relaxed);
}

namespace {
// Time into the sequence, and which output that falls on (-1 once done).
int identify_at(uint32_t* phase_ms) {
    const uint32_t mask = g_identify_mask.load(std::memory_order_acquire);
    if (!mask) return -1;
    const uint32_t slot = g_identify_blinks.load(std::memory_order_relaxed) * kIdentifyPeriodMs;
    const uint32_t now  = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t t    = now - g_identify_t0_ms.load(std::memory_order_relaxed);
    uint32_t idx        = t / slot;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        if (!(mask & (1u << ch))) continue;
        if (idx-- == 0) {
            if (phase_ms) *phase_ms = t % slot;
            return static_cast<int>(ch);
        }
    }
    g_identify_mask.store(0, std::memory_order_relaxed);  // the last one is done
    return -1;
}
}  // namespace

int identify_channel() {
    return identify_at(nullptr);
}

bool identify_lit() {
    uint32_t phase = 0;
    return identify_at(&phase) >= 0 && phase % kIdentifyPeriodMs < kIdentifyPeriodMs / 2;
}

void fseq_set_active(bool active) {
    g_fseq_active.store(active, std::memory_order_relaxed);
}

bool fseq_is_active() {
    return g_fseq_active.load(std::memory_order_relaxed);
}

namespace {

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Points output `o` at `to` (-1 = live), crossfading from what it shows now.
void assign_output(size_t o, int8_t to, int32_t fade_ms) {
    const int8_t from = g_scene_out[o].load(std::memory_order_relaxed);
    if (from == to) return;
    const uint32_t len = fade_ms < 0 ? scene_fade_ms() : static_cast<uint32_t>(fade_ms);
    g_fade_from[o].store(from, std::memory_order_relaxed);
    g_fade_start_ms[o].store(now_ms(), std::memory_order_relaxed);
    g_fade_len_ms[o].store(
        static_cast<uint16_t>(len > config::kMaxSceneFadeMs ? config::kMaxSceneFadeMs : len),
        std::memory_order_relaxed);
    g_scene_out[o].store(to, std::memory_order_release);
}

}  // namespace

namespace {

uint16_t fade_len(int32_t fade_ms) {
    const uint32_t len = fade_ms < 0 ? scene_fade_ms() : static_cast<uint32_t>(fade_ms);
    return static_cast<uint16_t>(len > config::kMaxSceneFadeMs ? config::kMaxSceneFadeMs : len);
}

// With g_play_mux held: fixture (o, f) to `play` (-1 = back to its output),
// crossfading; plays left without a fixture free their slot.
void set_owner_locked(size_t o, size_t f, int8_t play, uint16_t fade) {
    if (g_owner[o][f] == play) return;
    g_owner[o][f]    = play;
    g_fade_req[o][f] = fade;
}

void collect_plays_locked() {
    for (size_t p = 0; p < kMaxPlays; ++p) {
        if (g_plays[p].scene < 0) continue;
        bool used = false;
        for (size_t o = 0; o < config::kNumChannels && !used; ++o)
            for (size_t f = 0; f < config::kMaxFixtures && !used; ++f)
                used = g_owner[o][f] == static_cast<int8_t>(p);
        if (!used) g_plays[p] = Play{};
    }
}

// Releases every fixture whose owner matches `pred(play)` (or on `outputs`).
template <typename Pred> void release_locked(uint8_t outputs, Pred pred, uint16_t fade) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        for (size_t f = 0; f < config::kMaxFixtures; ++f) {
            const int8_t p = g_owner[o][f];
            if (p >= 0 && (((outputs >> o) & 1) || pred(g_plays[p])))
                set_owner_locked(o, f, -1, fade);
        }
    collect_plays_locked();
}

struct PlayLock {
    PlayLock() {
        if (g_play_mux) xSemaphoreTake(g_play_mux, portMAX_DELAY);
    }
    ~PlayLock() {
        if (g_play_mux) xSemaphoreGive(g_play_mux);
    }
};

}  // namespace

void group_play(uint8_t scene_index, uint8_t group, int32_t fade_ms) {
    if (scene_index >= config::num_scenes() || group >= config::get_groups().count) return;
    const config::FixtureGroup g = config::get_groups().groups[group];
    const uint16_t fade          = fade_len(fade_ms);
    PlayLock lock;
    int slot = -1;
    for (size_t p = 0; p < kMaxPlays && slot < 0; ++p)
        if (g_plays[p].scene == static_cast<int8_t>(scene_index) &&
            g_plays[p].group == static_cast<int8_t>(group))
            slot = static_cast<int>(p);
    for (size_t p = 0; p < kMaxPlays && slot < 0; ++p)
        if (g_plays[p].scene < 0) slot = static_cast<int>(p);
    if (slot < 0) {
        ESP_LOGW(TAG, "%u plays at once: no slot left", static_cast<unsigned>(kMaxPlays));
        return;
    }
    g_plays[slot] = Play{ static_cast<int8_t>(scene_index), static_cast<int8_t>(group) };
    for (size_t m = 0; m < g.count; ++m)
        set_owner_locked(g.members[m].output, g.members[m].fixture, static_cast<int8_t>(slot),
                         fade);
    collect_plays_locked();
}

void group_stop(uint8_t group, int32_t fade_ms) {
    PlayLock lock;
    release_locked(
        0, [group](const Play& p) { return p.group == static_cast<int8_t>(group); },
        fade_len(fade_ms));
}

int fixture_scene(size_t ch, size_t fixture) {
    if (ch >= config::kNumChannels || fixture >= config::kMaxFixtures) return -1;
    PlayLock lock;
    const int8_t p = g_owner[ch][fixture];
    return p < 0 ? -1 : g_plays[p].scene;
}

size_t active_plays(PlayInfo* out, size_t cap) {
    PlayLock lock;
    size_t n = 0;
    for (size_t p = 0; p < kMaxPlays; ++p)
        if (g_plays[p].scene >= 0) {
            if (n < cap) out[n] = PlayInfo{ g_plays[p].scene, g_plays[p].group };
            ++n;
        }
    return n;
}

uint32_t scene_fade_ms() {
    const int32_t desk = g_dmx_fade_ms.load(std::memory_order_relaxed);
    return desk >= 0 ? static_cast<uint32_t>(desk) : config::get_global().scene_fade_ms;
}

void scene_start(uint8_t scene_index) {
    scene_start_on(scene_index, kAllOutputs);
}

void scene_start_on(uint8_t scene_index, uint8_t outputs, int32_t fade_ms) {
    if (scene_index >= config::num_scenes()) return;
    config::Scene scene;
    config::copy_scene(scene_index, scene);
    // A scene with a default group, started everywhere: it plays on its group.
    const int group = config::scene_group(scene);
    if (outputs == kAllOutputs && group >= 0 && group < config::get_groups().count) {
        group_play(scene_index, static_cast<uint8_t>(group), fade_ms);
        return;
    }
    const uint8_t mask = outputs & config::scene_mask(scene);
    {
        PlayLock lock;  // the outputs it takes show it whole: their fixtures too
        release_locked(mask, [](const Play&) { return false; }, fade_len(fade_ms));
    }
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((mask >> o) & 1) assign_output(o, static_cast<int8_t>(scene_index), fade_ms);
}

void scene_stop() {
    scene_stop_on(kAllOutputs);
}

void scene_stop_on(uint8_t outputs, int32_t fade_ms) {
    {
        PlayLock lock;
        release_locked(outputs, [](const Play&) { return false; }, fade_len(fade_ms));
    }
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) assign_output(o, -1, fade_ms);
}

void scene_stop_scene(uint8_t scene_index) {
    {
        PlayLock lock;
        release_locked(
            0, [scene_index](const Play& p) { return p.scene == static_cast<int8_t>(scene_index); },
            fade_len(kDefaultFade));
    }
    scene_stop_on(scene_outputs(scene_index));
}

void scene_list_edited(config::SceneEdit op, size_t a, size_t b) {
    {
        PlayLock lock;  // plays follow their scene too (a deleted one stops)
        for (auto& p : g_plays)
            if (p.scene >= 0)
                p.scene = static_cast<int8_t>(config::remap_scene_index(p.scene, op, a, b));
        for (size_t o = 0; o < config::kNumChannels; ++o)
            for (size_t f = 0; f < config::kMaxFixtures; ++f)
                if (g_owner[o][f] >= 0 && g_plays[g_owner[o][f]].scene < 0) g_owner[o][f] = -1;
        collect_plays_locked();
    }
    for (size_t o = 0; o < config::kNumChannels; ++o) {
        g_scene_out[o].store(static_cast<int8_t>(config::remap_scene_index(
                                 g_scene_out[o].load(std::memory_order_relaxed), op, a, b)),
                             std::memory_order_relaxed);
        g_fade_from[o].store(static_cast<int8_t>(config::remap_scene_index(
                                 g_fade_from[o].load(std::memory_order_relaxed), op, a, b)),
                             std::memory_order_relaxed);
    }
}

int scene_on_output(size_t ch) {
    if (ch >= config::kNumChannels) return -1;
    return g_scene_out[ch].load(std::memory_order_relaxed);
}

uint8_t scene_outputs(uint8_t index) {
    uint8_t mask = 0;
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if (g_scene_out[o].load(std::memory_order_relaxed) == static_cast<int8_t>(index))
            mask |= static_cast<uint8_t>(1u << o);
    return mask;
}

int active_scene() {
    for (size_t o = 0; o < config::kNumChannels; ++o) {
        const int sc = g_scene_out[o].load(std::memory_order_relaxed);
        if (sc >= 0) return sc;
    }
    return -1;
}

// ── Show control ─────────────────────────────────────────────────────────────

void master_set(uint8_t outputs, uint16_t level) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) g_local_master[o].store(level, std::memory_order_relaxed);
}

uint16_t master_local(size_t ch) {
    return ch < config::kNumChannels ? g_local_master[ch].load(std::memory_order_relaxed)
                                     : kMasterFull;
}

uint16_t master_effective(size_t ch) {
    if (ch >= config::kNumChannels) return kMasterFull;
    const uint32_t l = g_local_master[ch].load(std::memory_order_relaxed);
    const uint32_t d = g_dmx_master[ch].load(std::memory_order_relaxed);
    return static_cast<uint16_t>(l * d / kMasterFull);
}

void blackout_set(uint8_t outputs, bool on) {
    if (on)
        g_local_blackout.fetch_or(outputs, std::memory_order_relaxed);
    else
        g_local_blackout.fetch_and(~static_cast<uint32_t>(outputs), std::memory_order_relaxed);
}

void blackout_toggle(uint8_t outputs) {
    const bool any_lit = (g_local_blackout.load(std::memory_order_relaxed) & outputs) != outputs;
    blackout_set(outputs, any_lit);
}

uint8_t blackout_local() {
    return static_cast<uint8_t>(g_local_blackout.load(std::memory_order_relaxed));
}

uint8_t blackout_effective() {
    return static_cast<uint8_t>(g_local_blackout.load(std::memory_order_relaxed) |
                                g_dmx_blackout.load(std::memory_order_relaxed));
}

void strobe_set(uint8_t outputs, uint8_t hz10) {
    if (hz10 > logic::kStrobeMaxHz10) hz10 = logic::kStrobeMaxHz10;
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) g_local_strobe[o].store(hz10, std::memory_order_relaxed);
}

uint8_t strobe_local(size_t ch) {
    return ch < config::kNumChannels ? g_local_strobe[ch].load(std::memory_order_relaxed) : 0;
}

uint8_t strobe_effective(size_t ch) {
    if (ch >= config::kNumChannels) return 0;
    const uint8_t l = g_local_strobe[ch].load(std::memory_order_relaxed);
    const uint8_t d = g_dmx_strobe[ch].load(std::memory_order_relaxed);
    return l > d ? l : d;
}

// ── Control universe ─────────────────────────────────────────────────────────

bool control_live() {
    if (g_ctrl_slot == logic::kNoSlot || !config::get_control().enabled) return false;
    const int64_t last = g_ctrl_last_us.load(std::memory_order_relaxed);
    return last != 0 && esp_timer_get_time() - last < kControlTimeoutUs;
}

int control_universe() {
    const auto& c = config::get_control();
    return c.enabled ? static_cast<int>(c.universe) : -1;
}

int control_pool_slot() {
    return g_ctrl_slot == logic::kNoSlot ? -1 : static_cast<int>(g_ctrl_slot);
}

int64_t control_last_rx_ms_ago() {
    const int64_t last = g_ctrl_last_us.load(std::memory_order_relaxed);
    return last == 0 ? -1 : (esp_timer_get_time() - last) / 1000;
}

int effect_for_dmx_value(uint8_t v) {
    return logic::effect_from_dmx(v);
}

int16_t take_fseq_request() {
    return static_cast<int16_t>(g_fseq_request.exchange(kFseqNoRequest, std::memory_order_acq_rel));
}

void update_show_control() {
    const bool live = control_live();
    if (!live) {
        if (g_ctrl_was_live) {
            // Never leave the rig dark or strobing because a cable came out;
            // the fade time goes back to the configured one.
            for (size_t o = 0; o < config::kNumChannels; ++o) {
                g_dmx_master[o].store(kMasterFull, std::memory_order_relaxed);
                g_dmx_strobe[o].store(0, std::memory_order_relaxed);
            }
            g_dmx_blackout.store(0, std::memory_order_relaxed);
            g_dmx_fade_ms.store(-1, std::memory_order_relaxed);
            for (auto& m : g_gmaster)
                m = kMasterFull;
            g_gblackout     = 0;
            g_ctrl_was_live = false;
            ESP_LOGW(TAG, "control universe lost: master/blackout/strobe released");
        }
        // Scene overrides survive a lost signal (the look holds), but not the
        // control universe being switched off.
        if (!config::get_control().enabled) {
            for (auto& o : g_ovr)
                o = logic::EffectOverride{};
            for (auto& o : g_govr)
                o = logic::EffectOverride{};
        }
        return;
    }
    const auto& c        = config::get_control();
    const uint8_t* front = g_uni_front.load(std::memory_order_acquire) +
                           static_cast<size_t>(g_ctrl_slot) * kUniverseSize;
    static logic::ControlEval ev;
    logic::evaluate_control(c, front, kUniverseSize, ev);

    for (size_t o = 0; o < config::kNumChannels; ++o) {
        g_dmx_master[o].store(ev.master[o], std::memory_order_relaxed);
        g_dmx_strobe[o].store(ev.strobe_hz10[o], std::memory_order_relaxed);
        g_ovr[o] = ev.ovr[o];
    }
    g_dmx_blackout.store(ev.blackout, std::memory_order_relaxed);
    g_dmx_fade_ms.store(ev.fade_ms, std::memory_order_relaxed);
    for (size_t gi = 0; gi < config::kMaxGroups; ++gi) {
        g_govr[gi]    = ev.govr[gi];
        g_gmaster[gi] = ev.gmaster[gi];
    }
    g_gblackout = ev.gblackout;

    const bool first = !g_ctrl_was_live;
    for (uint8_t i = 0; i < ev.n_scene; ++i) {
        const uint8_t band = ev.scene_band[i];
        if (!first && band == g_ctrl_prev_band[i]) continue;
        g_ctrl_prev_band[i] = band;
        const int group     = ev.scene_group[i];
        if (band == 0) {
            if (first) continue;
            if (group >= 0)
                group_stop(static_cast<uint8_t>(group));
            else
                scene_stop_on(ev.scene_mask[i]);
        } else if (band - 1u < config::num_scenes()) {
            if (group >= 0)
                group_play(static_cast<uint8_t>(band - 1), static_cast<uint8_t>(group));
            else
                scene_start_on(static_cast<uint8_t>(band - 1), ev.scene_mask[i]);
        }
    }
    if (ev.fseq_band >= 0 && (first || ev.fseq_band != g_ctrl_prev_fseq)) {
        if (ev.fseq_band > 0)
            g_fseq_request.store(static_cast<int16_t>(ev.fseq_band - 1), std::memory_order_release);
        else if (!first)
            g_fseq_request.store(kFseqStopRequest, std::memory_order_release);
        g_ctrl_prev_fseq = ev.fseq_band;
    }
    g_ctrl_was_live = true;
}

namespace {

// The network's view of the output into `buf`: its pixels decoded from the
// universes, or — in DMX control mode — its fixtures drawn from their channels.
void decode_live(size_t ch, const config::ChannelConfig& cc, uint8_t* buf, uint64_t t) {
    auto universe = [](uint16_t u) { return universe_front_buffer_for(u); };
    // Its pixels, then its fixtures over them — or alone on a dark strip. An
    // output driven neither way shows nothing of its own (scenes still play).
    const bool pixels = config::pixel_mapped(cc);
    if (pixels)
        logic::decode_pixels(buf, kMaxBytesPerChan, cc, universe);
    else if (!config::fixture_controlled(cc))
        std::memset(buf, 0, logic::channel_total_bytes(cc));
    if (config::fixture_controlled(cc))
        logic::render_fixtures(
            buf, kMaxBytesPerChan, cc, config::get_profiles(), t, universe,
            [](size_t index, config::Effect& e) { return config::copy_effect(index, e); }, pixels,
            g_fix_fx[ch], g_fx_scratch);
}

// The desk's pick of a look on an output or a group — its Effect (bank) and
// Generator channels — as a LookFade id, and back.
int32_t desk_look(const logic::EffectOverride& o) {
    return static_cast<uint8_t>(o.bank + 1) | (static_cast<uint8_t>(o.generator + 1) << 8);
}
logic::EffectOverride with_look(logic::EffectOverride o, int32_t look) {
    o.bank      = static_cast<int16_t>((look & 0xFF) - 1);
    o.generator = static_cast<int16_t>(((look >> 8) & 0xFF) - 1);
    return o;
}
uint16_t look_fade_ms() {
    const uint32_t ms = scene_fade_ms();
    return static_cast<uint16_t>(ms < 0xFFFF ? ms : 0xFFFF);
}

// Clock keys: a scene with the desk's look, and the failsafe scene.
uint32_t scene_key(int src, int32_t look) {
    return (static_cast<uint32_t>(src + 1) << 16) | static_cast<uint16_t>(look);
}
constexpr uint32_t kFailsafeKey = 0x80000000u;

// Scene `src`'s part on output `ch` into `buf`, in the desk's `look` with its
// other overrides; false when the scene has no part there.
bool draw_scene(size_t ch, const config::ChannelConfig& cc, int src, int32_t look, uint8_t* buf,
                uint64_t t) {
    // A copy: a list edit (memmove) on another task cannot tear the part or
    // the effect being drawn.
    config::Effect effect;
    uint8_t mode;
    if (!config::copy_scene_part(static_cast<size_t>(src), ch, effect, mode)) return false;
    const logic::EffectOverride o = with_look(g_ovr[ch], look);
    // The desk's Bank channel: another effect of the bank on this output (a
    // band past the bank keeps the scene's own).
    config::Effect picked;
    if (o.bank >= 0 && config::copy_effect(static_cast<size_t>(o.bank), picked)) effect = picked;
    logic::apply_effect_override(effect, o);
    mode                   = logic::apply_mode_override(mode, o);
    const uint32_t key     = scene_key(src, look);
    const logic::FxTime at = logic::clock_tick(logic::clock_slot(g_out_clock[ch], 4, key), key,
                                               effect, t);
    logic::fill_effect_on_channel(buf, kMaxBytesPerChan, cc, led::bytes_per_pixel(cc.protocol),
                                  effect, mode, at);
    return true;
}

// One source into `buf`: scene `src` (with the desk's overrides) when it is a
// valid scene whose mask still holds the output, else the live path — FSEQ,
// failsafe or the decoded universes.
void render_source(size_t ch, const config::ChannelConfig& cc, int src, uint8_t* buf, uint64_t t) {
    const uint8_t bpp = led::bytes_per_pixel(cc.protocol);
    if (src >= 0 && static_cast<size_t>(src) < config::num_scenes()) {
        const logic::LookFade& lf = g_out_look[ch];
        if (draw_scene(ch, cc, src, lf.shown, buf, t)) {
            const auto now = static_cast<uint32_t>(t);
            if (logic::look_fading(lf, now) && g_fx_scratch &&
                draw_scene(ch, cc, src, lf.from, g_fx_scratch, t))
                logic::blend_into(buf, g_fx_scratch, static_cast<size_t>(cc.pixel_count) * bpp,
                                  logic::fade_weight(now - lf.start, lf.len));
            return;
        }
    }

    // FSEQ playback: data is already in the universe banks via inject_universe().
    // Suppress the failsafe check while FSEQ is active so a seek/block-load
    // pause doesn't momentarily blackout channels that are being played back.
    if (g_fseq_active.load(std::memory_order_relaxed)) {
        decode_live(ch, cc, buf, t);
        return;
    }

    // Signal-loss failsafe: a channel silent past the timeout stops decoding
    // (stale) universes and emits the fallback instead. Hold mode never gets
    // here — stale decode IS the hold.
    const auto& g = config::get_global();
    if (g.failsafe_mode != config::kFailsafeHold &&
        logic::failsafe_due(g_last_activity_us[ch].load(std::memory_order_relaxed),
                            esp_timer_get_time(), g.failsafe_timeout_s)) {
        // Mode "scene": play the configured scene's effect on the lost channel —
        // only on the channels the scene targets; the others black out.
        config::Effect effect;
        uint8_t fx_mode;
        if (g.failsafe_mode == config::kFailsafeScene &&
            config::copy_scene_part(g.failsafe_scene, ch, effect, fx_mode)) {
            const uint32_t key     = kFailsafeKey | g.failsafe_scene;
            const logic::FxTime at = logic::clock_tick(logic::clock_slot(g_out_clock[ch], 4, key),
                                                       key, effect, t);
            logic::fill_effect_on_channel(buf, kMaxBytesPerChan, cc, bpp, effect, fx_mode, at);
            return;
        }
        const uint8_t mode = g.failsafe_mode == config::kFailsafeScene ? config::kFailsafeBlackout
                                                                       : g.failsafe_mode;
        logic::fill_failsafe_pattern(buf, kMaxBytesPerChan, cc.pixel_count, bpp, mode, g.failsafe_r,
                                     g.failsafe_g, g.failsafe_b);
        return;
    }

    decode_live(ch, cc, buf, t);
}

}  // namespace

namespace {

// Play `p` along its group's strip into `strip`, in the desk's `look` with its
// other overrides on the group: the scene's look, its first part's effect
// and mode. False when the scene has no part.
bool draw_play(size_t p, const Play& pl, size_t members, int32_t look, uint8_t* strip, uint64_t t) {
    config::Effect effect;
    uint8_t mode;
    if (!config::copy_scene_look(static_cast<size_t>(pl.scene), effect, mode)) return false;
    const logic::EffectOverride o = with_look(g_govr[pl.group], look);  // the desk, on this group
    config::Effect picked;
    if (o.bank >= 0 && config::copy_effect(static_cast<size_t>(o.bank), picked)) effect = picked;
    logic::apply_effect_override(effect, o);
    mode           = logic::apply_mode_override(mode, o);
    const auto key = (static_cast<uint32_t>(pl.group) << 24) | scene_key(pl.scene, look);
    return logic::render_group_strip(
               strip, kMaxStripPx * 3, g_play_lens[p], members, effect, mode,
               logic::clock_tick(logic::clock_slot(g_play_clock[p], 2, key), key, effect, t)) > 0;
}

// Render task, once a frame: this frame's plays, owners and fade requests, the
// fixture spans of every output, and each play's strip drawn once.
void plays_frame_begin(uint64_t t) {
    g_frame_ms = t;
    ++g_frame_seq;
    {
        PlayLock lock;
        std::memcpy(g_plays_r, g_plays, sizeof(g_plays));
        std::memcpy(g_owner_r, g_owner, sizeof(g_owner));
        std::memcpy(g_fade_req_r, g_fade_req, sizeof(g_fade_req));
        for (auto& row : g_fade_req)
            for (auto& r : row)
                r = kNoFadeReq;
    }
    for (size_t o = 0; o < config::kNumChannels; ++o)
        logic::fixture_spans_by_index(effective_channel(o), g_spans_r[o]);
    const auto& groups = config::get_groups();
    const auto now     = static_cast<uint32_t>(t);
    for (size_t p = 0; p < kMaxPlays; ++p) {
        g_play_ok[p]  = false;
        const Play pl = g_plays_r[p];
        if (pl.scene != g_play_was[p].scene || pl.group != g_play_was[p].group)
            g_play_look[p] = logic::LookFade{};  // another play: no fade from the last one
        g_play_was[p] = pl;
        if (pl.scene < 0 || pl.group < 0 || pl.group >= groups.count || !g_play_strip[p] ||
            static_cast<size_t>(pl.scene) >= config::num_scenes())
            continue;
        g_group_r[p]  = groups.groups[pl.group];
        const auto& g = g_group_r[p];
        uint32_t at   = 0;
        for (size_t m = 0; m < g.count; ++m) {
            const auto& r      = g.members[m];
            g_play_offs[p][m]  = at;
            g_play_lens[p][m]  = g_spans_r[r.output][r.fixture].count;
            at                += g_play_lens[p][m];
        }
        // The desk's look on this group, faded from the last over the scene
        // fade time.
        logic::LookFade& lf = g_play_look[p];
        logic::look_tick(lf, desk_look(g_govr[pl.group]), look_fade_ms(), now);
        g_play_ok[p] = draw_play(p, pl, g.count, lf.shown, g_play_strip[p], t);
        if (g_play_ok[p] && logic::look_fading(lf, now) && g_play_scratch &&
            draw_play(p, pl, g.count, lf.from, g_play_scratch, t))
            logic::blend_into(g_play_strip[p], g_play_scratch, static_cast<size_t>(at) * 3,
                              logic::fade_weight(now - lf.start, lf.len));
    }
}

// The plays' slices over output `ch`, then the per-fixture crossfades from
// what each changed fixture showed.
void overlay_plays(size_t ch, uint8_t* dst, uint8_t bpp, uint64_t t) {
    const uint8_t* front = pixel_front_buffer(ch);
    for (size_t f = 0; f < config::kMaxFixtures; ++f) {
        const logic::Span& sp = g_spans_r[ch][f];
        if (sp.count == 0) continue;
        const size_t at    = static_cast<size_t>(sp.first) * bpp;
        const size_t bytes = static_cast<size_t>(sp.count) * bpp;
        if (g_fade_req_r[ch][f] != kNoFadeReq) {  // its owner changed: fade from what it shows
            g_fix_fade_len[ch][f]   = g_fade_req_r[ch][f];
            g_fix_fade_start[ch][f] = static_cast<uint32_t>(t);
            if (front && g_fix_snap[ch]) std::memcpy(g_fix_snap[ch] + at, front + at, bytes);
        }
        const int8_t p = g_owner_r[ch][f];
        if (p >= 0 && g_play_ok[p]) {
            const auto& g = g_group_r[p];
            for (size_t m = 0; m < g.count; ++m)
                if (g.members[m].output == ch && g.members[m].fixture == f) {
                    logic::put_member(dst, bpp, sp, g_play_strip[p] + g_play_offs[p][m] * 3,
                                      g_play_lens[p][m]);
                    break;
                }
        }
        const uint16_t len = g_fix_fade_len[ch][f];
        if (len && g_fix_snap[ch]) {
            const uint32_t elapsed = static_cast<uint32_t>(t) - g_fix_fade_start[ch][f];
            if (elapsed < len)
                logic::blend_into(dst + at, g_fix_snap[ch] + at, bytes,
                                  logic::fade_weight(elapsed, len));
            else
                g_fix_fade_len[ch][f] = 0;
        }
    }
}

// A desk's group master / blackout over that group's bars on output `ch`.
void dim_groups(size_t ch, uint8_t* dst, uint8_t bpp) {
    const auto& groups = config::get_groups();
    for (size_t gi = 0; gi < groups.count; ++gi) {
        const bool dark      = (g_gblackout >> gi) & 1;
        const uint16_t level = dark ? 0 : g_gmaster[gi];
        if (level == kMasterFull) continue;
        const auto& g = groups.groups[gi];
        for (size_t m = 0; m < g.count; ++m) {
            if (g.members[m].output != ch) continue;
            const logic::Span& sp = g_spans_r[ch][g.members[m].fixture];
            if (sp.count)
                logic::apply_master(dst + static_cast<size_t>(sp.first) * bpp,
                                    static_cast<size_t>(sp.count) * bpp, level);
        }
    }
}

}  // namespace

namespace {

// The time output `ch` draws its frame at: the swap's, once.
uint64_t frame_ms(size_t ch) {
    if (g_ch_seq[ch] != g_frame_seq) {
        g_ch_seq[ch] = g_frame_seq;
        return g_frame_ms;
    }
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace

bool decode_pixels_for_channel(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    uint8_t* dst = pixel_back_buffer(ch);
    if (!dst) return false;
    const config::ChannelConfig cc = effective_channel(ch);

    // Identify blink: top priority — it answers "which strip is this?".
    if (identify_channel() == static_cast<int>(ch)) {
        const uint8_t bpp = led::bytes_per_pixel(cc.protocol);
        const uint8_t lvl = identify_lit() ? 255 : 0;  // 2 Hz blink, starting lit
        logic::fill_failsafe_pattern(dst, kMaxBytesPerChan, cc.pixel_count, bpp,
                                     config::kFailsafeColor, lvl, lvl, lvl);
        return true;
    }

    const uint32_t preview = g_pixel_preview.load(std::memory_order_relaxed);
    if ((preview >> 16) == ch) {
        const uint16_t count = static_cast<uint16_t>(preview & 0xFFFFu);
        // Consume any owed shrink-erase here so exactly this frame carries the
        // black tail; the emit count is published for the output stage to size
        // the emission so the dropped LEDs are actually clocked out once.
        const auto erase = static_cast<uint16_t>(
            g_preview_erase.exchange(0, std::memory_order_relaxed));
        const uint16_t emit      = erase > count ? erase : count;
        const bool edit          = g_preview_gaps_on.load(std::memory_order_acquire);
        const led::PixelGap* gps = edit ? g_preview_gaps : cc.gaps;
        const size_t ngaps       = led::gap_count(gps, led::kMaxPixelGaps);
        // The ruler is written in physical order (gaps painted in place), so
        // the output stage emits the physical count with no gap mapping.
        const uint32_t phys = led::physical_count(emit, gps, ngaps);
        g_preview_emit_next.store(
            static_cast<uint16_t>(phys < kMaxPixelsPerChan ? phys : kMaxPixelsPerChan),
            std::memory_order_relaxed);
        logic::fill_preview_pattern(dst, kMaxBytesPerChan, count, emit,
                                    led::bytes_per_pixel(cc.protocol), gps, ngaps);
        return true;
    }

    // Effects and strobe: 64-bit ms, no wrap in a lifetime. The fade keeps a
    // 32-bit stamp — a difference, so its wrap is harmless.
    const uint64_t t   = frame_ms(ch);
    const size_t bytes = static_cast<size_t>(cc.pixel_count) * led::bytes_per_pixel(cc.protocol);
    // The desk's look on this output, faded from the last over the scene fade.
    logic::look_tick(g_out_look[ch], desk_look(g_ovr[ch]), look_fade_ms(),
                     static_cast<uint32_t>(t));
    render_source(ch, cc, g_scene_out[ch].load(std::memory_order_acquire), dst, t);

    // Crossfade from what the output showed before its last scene change.
    const uint16_t len = g_fade_len_ms[ch].load(std::memory_order_relaxed);
    if (len) {
        const uint32_t elapsed = static_cast<uint32_t>(t) -
                                 g_fade_start_ms[ch].load(std::memory_order_relaxed);
        if (elapsed < len && g_scratch) {
            render_source(ch, cc, g_fade_from[ch].load(std::memory_order_relaxed), g_scratch, t);
            logic::blend_into(dst, g_scratch, bytes, logic::fade_weight(elapsed, len));
        } else {
            g_fade_len_ms[ch].store(0, std::memory_order_relaxed);
        }
    }

    // Scenes on groups: their fixtures, over whatever the output shows.
    overlay_plays(ch, dst, led::bytes_per_pixel(cc.protocol), t);
    dim_groups(ch, dst, led::bytes_per_pixel(cc.protocol));

    // Show control last: it dims whatever the output renders.
    if (((blackout_effective() >> ch) & 1) || !logic::strobe_lit(t, strobe_effective(ch)))
        std::memset(dst, 0, bytes);
    else
        logic::apply_master(dst, bytes, master_effective(ch));
    return true;
}

size_t render_effect_preview(const config::Effect& effect, uint16_t pixels, uint64_t t_ms,
                             uint8_t* rgb, size_t cap) {
    if (!rgb || pixels == 0 || static_cast<size_t>(pixels) * 3 > cap) return 0;
    logic::fill_effect_run(rgb, cap, pixels, 3, effect, t_ms);
    return pixels;
}

bool is_channel_capacity_ok(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    return g_channel_capacity_ok[ch];
}

void validate_capacity() {
    const uint8_t refresh = config::get_global().refresh_rate_hz;
    if (refresh == 0) return;

    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc            = config::get_channel(ch);
        const uint16_t emitted    = effective_channel(ch).pixel_count;
        const bool ok             = emitted >= cc.pixel_count;
        g_channel_capacity_ok[ch] = ok;
        if (!ok) {
            ESP_LOGW(TAG, "ch %zu: %u px requested, %u emitted at %u Hz (proto=%d)", ch,
                     static_cast<unsigned>(cc.pixel_count), static_cast<unsigned>(emitted),
                     static_cast<unsigned>(refresh), static_cast<int>(cc.protocol));
        }
    }
}

uint16_t channel_max_pixels(size_t ch) {
    if (ch >= config::kNumChannels) return 0;
    const uint8_t refresh = config::get_global().refresh_rate_hz;
    return logic::max_live_pixels_for(config::get_channel(ch), led::kPclkHz, refresh,
                                      led::kMaxSamplesPerFrame);
}

uint32_t physical_pixels(const config::ChannelConfig& cc) {
    return logic::channel_physical_pixels(cc);
}

size_t channel_gap_count(size_t ch) {
    if (ch >= config::kNumChannels) return 0;
    return logic::channel_gap_count(config::get_channel(ch));
}

config::ChannelConfig effective_channel(size_t ch) {
    config::ChannelConfig cc = config::get_channel(ch < config::kNumChannels ? ch : 0);
    cc.pixel_count           = logic::effective_pixel_count(
        cc, led::kPclkHz, config::get_global().refresh_rate_hz, led::kMaxSamplesPerFrame);
    return cc;
}

uint64_t frame_emit_us() {
    uint64_t longest = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const uint64_t t = logic::channel_t_dma_us(effective_channel(ch), led::kPclkHz);
        if (t > longest) longest = t;
    }
    return longest;
}

namespace {
// Every output a pool slot feeds received data now.
void note_slot_activity(uint16_t slot) {
    const int64_t now = esp_timer_get_time();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        if (g_slot_chans[slot] & (1u << ch))
            g_last_activity_us[ch].store(now, std::memory_order_relaxed);
}
}  // namespace

int channel_for_universe(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot || g_slot_chans[slot] == 0) return -1;
    for (int ch = 0; ch < static_cast<int>(config::kNumChannels); ++ch)  // the lowest
        if (g_slot_chans[slot] & (1u << ch)) return ch;
    return -1;
}

void note_universe_activity(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return;
    note_slot_activity(slot);
}

void note_channel_activity(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return;
    g_last_activity_us[channel_index].store(esp_timer_get_time(), std::memory_order_relaxed);
}

bool is_channel_active(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const int64_t last = g_last_activity_us[channel_index].load(std::memory_order_relaxed);
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < kActivityWindowUs;
}

bool is_channel_failsafe(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const auto& g = config::get_global();
    if (g.failsafe_mode == config::kFailsafeHold) return false;
    return logic::failsafe_due(g_last_activity_us[channel_index].load(std::memory_order_relaxed),
                               esp_timer_get_time(), g.failsafe_timeout_s);
}

void note_universe_terminated(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return;
    // Age the timestamp far into the past: still "was active once" (non-zero),
    // but past any timeout whatever the uptime — ageing it to boot+1 µs left a
    // stream terminated within the first failsafe_timeout_s of uptime ignored.
    // Every output the universe feeds (compact patching can share one).
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        if ((g_slot_chans[slot] & (1u << ch)) &&
            g_last_activity_us[ch].load(std::memory_order_relaxed) != 0)
            g_last_activity_us[ch].store(kTerminatedUs, std::memory_order_relaxed);
}

// Seed a pool slot in the back bank from the front bank the first time it is
// written after a swap, and mark it dirty so the next swap actually happens.
// The back bank is whichever one the front pointer is not on. Call with
// g_uni_swap_mux held, so the answer cannot go stale before it is used.
uint8_t* back_bank_locked() {
    return g_uni_front.load(std::memory_order_relaxed) == g_uni_bank_a ? g_uni_bank_b
                                                                       : g_uni_bank_a;
}

// Seed a pool slot in the back bank from the front bank the first time it is
// written after a swap, and mark it dirty so the next swap actually happens.
// Call with g_uni_swap_mux held.
void prepare_back_slot(uint16_t slot, uint8_t* back) {
    const uint32_t bit = 1u << (slot % 32);
    if (g_uni_dirty[slot / 32].fetch_or(bit, std::memory_order_acq_rel) & bit) return;
    const uint8_t* front = g_uni_front.load(std::memory_order_relaxed);
    const size_t base    = static_cast<size_t>(slot) * kUniverseSize;
    memcpy(back + base, front + base, kUniverseSize);
}

bool write_universe_from_source(uint16_t universe_number, const uint8_t* data, size_t len,
                                uint32_t source_id, int64_t timeout_us) {
    if (!data || !g_merge_staging || !g_uni_swap_mux) return false;
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return false;
    const bool ltp = config::get_global().merge_mode == config::kMergeLtp;

    // Seed + ingest under the swap lock: a swap landing between resolving the
    // back bank and finishing the write would put this frame into the bank the
    // decode is reading.
    xSemaphoreTake(g_uni_swap_mux, portMAX_DELAY);
    uint8_t* back = back_bank_locked();
    prepare_back_slot(slot, back);
    const bool ok = logic::merge_ingest(
        g_merge[slot], g_merge_staging + static_cast<size_t>(slot) * 2 * kUniverseSize,
        back + static_cast<size_t>(slot) * kUniverseSize, data, len, source_id, ltp,
        esp_timer_get_time(), timeout_us);
    xSemaphoreGive(g_uni_swap_mux);
    if (ok && slot == g_ctrl_slot)
        g_ctrl_last_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return ok;
}

void merge_drop_source(uint16_t universe_number, uint32_t source_id) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return;
    logic::merge_drop(g_merge[slot], source_id);
}

void merge_reset_universe(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return;
    std::memset(&g_merge[slot], 0, sizeof(g_merge[slot]));
}

void merge_cancel_all() {
    std::memset(g_merge, 0, sizeof(g_merge));
}

bool is_channel_merging(size_t channel_index) {
    if (channel_index >= config::kNumChannels) return false;
    const int64_t now = esp_timer_get_time();
    for (uint16_t slot = 0; slot < g_slots_used; ++slot) {
        if (!(g_slot_chans[slot] & (1u << channel_index))) continue;
        logic::MergeState m = g_merge[slot];
        logic::merge_expire(m, now, kArtnetMergeTimeoutUs);
        if (logic::merge_active_count(m) == 2) return true;
    }
    return false;
}

const uint8_t* universe_front_buffer_for(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return nullptr;
    return g_uni_front.load(std::memory_order_acquire) + slot * kUniverseSize;
}

bool inject_universe(uint16_t universe_number, size_t offset, const uint8_t* data, size_t len) {
    if (!data) return false;
    if (offset + len > kUniverseSize) return false;
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return false;
    const size_t base = static_cast<size_t>(slot) * kUniverseSize + offset;
    // Byte-level tearing against a concurrent artnet write or render read is
    // acceptable here — this is a bench/test path, not a sync-critical one.
    memcpy(g_uni_bank_a + base, data, len);
    memcpy(g_uni_bank_b + base, data, len);
    note_slot_activity(slot);
    if (slot == g_ctrl_slot) g_ctrl_last_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return true;
}

namespace {
bool g_in_frame = false;  // inside inject_frame_begin()/end() (the FSEQ task's only)
}  // namespace

void inject_frame_begin() {
    if (!g_uni_swap_mux) return;
    xSemaphoreTake(g_uni_swap_mux, portMAX_DELAY);
    g_in_frame = true;
}

bool inject_frame_universe(uint16_t universe_number, size_t offset, const uint8_t* data,
                           size_t len) {
    if (!g_in_frame || !data || offset + len > kUniverseSize) return false;
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot) return false;
    uint8_t* back = back_bank_locked();
    prepare_back_slot(slot, back);
    memcpy(back + static_cast<size_t>(slot) * kUniverseSize + offset, data, len);
    note_slot_activity(slot);
    if (slot == g_ctrl_slot) g_ctrl_last_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return true;
}

void inject_frame_end() {
    if (!g_in_frame) return;
    g_in_frame = false;
    xSemaphoreGive(g_uni_swap_mux);
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
namespace {
uint32_t sync_now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
void extend_sync(uint32_t timeout_ms) {
    const uint32_t until = sync_now_ms() + timeout_ms;
    const uint32_t cur   = g_sync_until_ms.load(std::memory_order_relaxed);
    if (!g_sync_seen.load(std::memory_order_relaxed) || static_cast<int32_t>(until - cur) > 0)
        g_sync_until_ms.store(until, std::memory_order_relaxed);
    g_sync_seen.store(1, std::memory_order_relaxed);
}
}  // namespace

void note_sync(uint32_t timeout_ms) {
    extend_sync(timeout_ms);
    g_sync_pending.store(1, std::memory_order_release);
    if (g_sync_sem) xSemaphoreGive(g_sync_sem);
}

void note_sync_hold(uint32_t timeout_ms, bool force) {
    extend_sync(timeout_ms);
    g_sync_force.store(force ? 1 : 0, std::memory_order_relaxed);
}

void note_sync_released() {
    g_sync_force.store(0, std::memory_order_relaxed);
}

void sync_reset() {
    g_sync_seen.store(0, std::memory_order_relaxed);
    g_sync_force.store(0, std::memory_order_relaxed);
    g_sync_pending.store(0, std::memory_order_relaxed);
}

bool sync_mode() {
    if (!g_sync_seen.load(std::memory_order_relaxed)) return false;
    if (g_sync_force.load(std::memory_order_relaxed)) return true;
    return static_cast<int32_t>(g_sync_until_ms.load(std::memory_order_relaxed) - sync_now_ms()) >
           0;
}

bool wait_for_sync_or_period(uint32_t period_ticks) {
    if (!g_sync_sem) {
        vTaskDelay(period_ticks);
        return false;
    }
    return xSemaphoreTake(g_sync_sem, period_ticks) == pdTRUE;
}

void swap_universes() {
    plays_frame_begin(static_cast<uint64_t>(esp_timer_get_time() / 1000));
    // Nothing arrived since the last swap: keep presenting the current front.
    // Swapping here would re-present the other bank, one source frame behind,
    // which is what made a 10 Hz source flicker between its last two frames.
    // Hold-last-value is the stage-lighting convention, and now it really holds:
    // a channel that has not received an update keeps its previous value because
    // the front bank is left alone, not because the banks take turns.
    // A sync is consumed even with nothing new: the next data must wait for its
    // own sync, not ride this one.
    const bool synced = g_sync_pending.exchange(0, std::memory_order_acq_rel) != 0;
    if (!any_dirty() || !g_uni_swap_mux) return;
    // Sync mode: what arrived waits in the back bank for the controller's sync,
    // so every universe of a frame goes out together.
    if (sync_mode() && !synced) return;
    xSemaphoreTake(g_uni_swap_mux, portMAX_DELAY);
    uint8_t* back = back_bank_locked();
    level_back_bank(back, g_uni_front.load(std::memory_order_relaxed));
    g_uni_front.store(back, std::memory_order_release);
    clear_dirty();
    xSemaphoreGive(g_uni_swap_mux);
}

uint8_t* pixel_back_buffer(size_t ch) {
    if (ch >= config::kNumChannels) return nullptr;
    return g_chan_bufs[ch].back;
}

const uint8_t* pixel_front_buffer(size_t ch) {
    if (ch >= config::kNumChannels) return nullptr;
    return g_chan_bufs[ch].front.load(std::memory_order_acquire);
}

size_t output_preview(size_t ch, uint8_t* rgb, size_t max_samples) {
    if (ch >= config::kNumChannels) return 0;
    const config::ChannelConfig cc = effective_channel(ch);
    return logic::downsample_rgb(pixel_front_buffer(ch), cc.pixel_count,
                                 static_cast<uint8_t>(led::bytes_per_pixel(cc.protocol)), rgb,
                                 max_samples);
}

void swap_pixels(size_t ch) {
    if (ch >= config::kNumChannels) return;
    if (pixel_preview_channel() == static_cast<int>(ch))  // the ruler's length goes with its frame
        g_preview_emit.store(g_preview_emit_next.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    uint8_t* new_front   = g_chan_bufs[ch].back;
    g_chan_bufs[ch].back = g_chan_bufs[ch].front.exchange(new_front, std::memory_order_acq_rel);
}

void publish_pixels() {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        swap_pixels(ch);
}

Stats get_stats() {
    Stats s = g_stats;
    return s;
}

void set_current_fps(uint32_t fps) {
    g_stats.current_fps = fps;
}
void set_decode_time(uint32_t last_us, uint32_t max_us) {
    g_stats.decode_us     = last_us;
    g_stats.decode_max_us = max_us;
}
void set_cpu_load(int core, uint8_t pct) {
    if (core == 0 || core == 1) g_stats.cpu_load[core] = pct;
}
uint8_t cpu_load_pct(uint32_t idle_us, uint32_t window_us) {
    if (window_us == 0) return kCpuLoadUnknown;
    if (idle_us >= window_us) return 0;
    return static_cast<uint8_t>(100u - static_cast<uint64_t>(idle_us) * 100u / window_us);
}
void note_frame_emitted() {
    __atomic_add_fetch(&g_stats.frames_emitted, 1, __ATOMIC_RELAXED);
}
void note_dma_underrun() {
    __atomic_add_fetch(&g_stats.dma_underruns, 1, __ATOMIC_RELAXED);
}

}  // namespace pixfrog::dmx
