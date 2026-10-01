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
// only across one universe-sized memcpy, so the render task's worst-case wait
// here is microseconds.
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
// Seeding on the receiver task rather than inside the swap is what keeps it
// race-free: only the receiver ever writes the back bank.
static_assert(kNumUniverses <= 64, "the dirty mask is a single uint64_t");
std::atomic<uint64_t> g_uni_dirty{ 0 };

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

// Pixel-count shrink erase: when the previewed count drops, the LEDs it just
// dropped hold their last value until overwritten. `g_preview_erase` is the
// extent the UI owes a black tail down to; the render task consumes it on the
// next decode so exactly one frame carries the erase. `g_preview_emit` is the
// physical LED count that frame emits (= max(count, erase)), published by
// decode for the output stage to read back.
std::atomic<uint32_t> g_preview_erase{ 0 };  // RMW: 32-bit (see g_local_blackout)
// Gap-edit override for the ruler (set from ui_task, read by render_task; a
// torn copy costs one oddly-marked preview frame).
led::PixelGap g_preview_gaps[led::kMaxPixelGaps]{};
std::atomic<bool> g_preview_gaps_on{ false };
std::atomic<uint16_t> g_preview_emit{ 0 };

// Scene per output (-1 = live input) and its crossfade: the source it fades
// from (-1 = live), when it started and how long it lasts. Written by any
// task, read by render_task; a torn set costs one oddly-blended frame.
std::atomic<int8_t> g_scene_out[config::kNumChannels];
std::atomic<int8_t> g_fade_from[config::kNumChannels];
std::atomic<uint32_t> g_fade_start_ms[config::kNumChannels];
std::atomic<uint16_t> g_fade_len_ms[config::kNumChannels];
// Crossfade source render target (render task only), internal SRAM.
uint8_t* g_scratch = nullptr;

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
logic::SceneOverride g_ovr[config::kNumChannels];
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
std::atomic<int8_t> g_identify_ch{ -1 };
std::atomic<uint32_t> g_identify_until_ms{ 0 };

// Universe → slot lookup, indexed by the flat 15-bit Art-Net Port-Address.
// Sized for the whole addressable range so routing is a single load; every
// read goes through slot_for_universe(), which rejects the out-of-range
// numbers sACN and FSEQ can produce.
uint16_t g_universe_to_slot[logic::kMaxUniverseNumber + 1]{};
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
    config::ChannelConfig chans[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        chans[ch] = config::get_channel(ch);

    size_t unmapped = 0;
    g_slots_used    = logic::build_universe_map(chans, config::kNumChannels, g_universe_to_slot,
                                                g_slot_to_channel, kNumUniverses, &unmapped);
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
            g_slot_to_channel[g_slots_used]   = kNoChannel;
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
    g_uni_dirty.store(0, std::memory_order_relaxed);

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

bool auto_patch_universes(uint16_t base, uint16_t* next_free) {
    config::ChannelConfig chans[config::kNumChannels];
    for (size_t i = 0; i < config::kNumChannels; ++i)
        chans[i] = config::get_channel(i);

    uint16_t starts[config::kNumChannels];
    const uint16_t next = logic::compute_auto_patch(base, chans, config::kNumChannels, starts);
    if (next_free) *next_free = next;

    bool all_persisted = true;
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        chans[i].universe_start  = starts[i];
        chans[i].dmx_start       = 1;  // cascade places every channel universe-aligned
        all_persisted           &= config::set_channel(i, chans[i]);
        mark_channel_dirty(i);
    }
    return all_persisted;
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

uint32_t scene_fade_ms() {
    const int32_t desk = g_dmx_fade_ms.load(std::memory_order_relaxed);
    return desk >= 0 ? static_cast<uint32_t>(desk) : config::get_global().scene_fade_ms;
}

void scene_start(uint8_t scene_index) {
    scene_start_on(scene_index, kAllOutputs);
}

void scene_start_on(uint8_t scene_index, uint8_t outputs, int32_t fade_ms) {
    if (scene_index >= config::num_scenes()) return;
    const uint8_t mask = outputs & config::get_scene(scene_index).channel_mask;
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((mask >> o) & 1) assign_output(o, static_cast<int8_t>(scene_index), fade_ms);
}

void scene_stop() {
    scene_stop_on(kAllOutputs);
}

void scene_stop_on(uint8_t outputs, int32_t fade_ms) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) assign_output(o, -1, fade_ms);
}

void scene_stop_scene(uint8_t scene_index) {
    scene_stop_on(scene_outputs(scene_index));
}

void scene_list_edited(config::SceneEdit op, size_t a, size_t b) {
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
            g_ctrl_was_live = false;
            ESP_LOGW(TAG, "control universe lost: master/blackout/strobe released");
        }
        // Scene overrides survive a lost signal (the look holds), but not the
        // control universe being switched off.
        if (!config::get_control().enabled)
            for (auto& o : g_ovr)
                o = logic::SceneOverride{};
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

    const bool first = !g_ctrl_was_live;
    for (uint8_t i = 0; i < ev.n_scene; ++i) {
        const uint8_t band = ev.scene_band[i];
        if (!first && band == g_ctrl_prev_band[i]) continue;
        g_ctrl_prev_band[i] = band;
        if (band == 0) {
            if (!first) scene_stop_on(ev.scene_mask[i]);
        } else if (band - 1u < config::num_scenes()) {
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

// One source into `buf`: scene `src` (with the desk's overrides) when it is a
// valid scene whose mask still holds the output, else the live path — FSEQ,
// failsafe or the decoded universes.
void render_source(size_t ch, const config::ChannelConfig& cc, int src, uint8_t* buf, uint64_t t) {
    const uint8_t bpp = led::bytes_per_pixel(cc.protocol);
    if (src >= 0 && static_cast<size_t>(src) < config::num_scenes()) {
        // A copy under the config lock: a scene-list edit (memmove) on another
        // task cannot tear the scene being drawn.
        config::Scene scene;
        config::copy_scene(static_cast<size_t>(src), scene);
        if ((scene.channel_mask >> ch) & 1) {
            logic::apply_scene_override(scene, g_ovr[ch]);
            logic::fill_scene_pattern(buf, kMaxBytesPerChan, cc.pixel_count, bpp, scene, t);
            return;
        }
    }

    // FSEQ playback: data is already in the universe banks via inject_universe().
    // Suppress the failsafe check while FSEQ is active so a seek/block-load
    // pause doesn't momentarily blackout channels that are being played back.
    if (g_fseq_active.load(std::memory_order_relaxed)) {
        logic::decode_pixels(buf, kMaxBytesPerChan, cc,
                             [](uint16_t u) { return universe_front_buffer_for(u); });
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
        config::Scene scene;
        config::copy_scene(g.failsafe_scene, scene);
        const bool in_mask = (scene.channel_mask >> ch) & 1;
        if (g.failsafe_mode == config::kFailsafeScene && in_mask) {
            logic::fill_scene_pattern(buf, kMaxBytesPerChan, cc.pixel_count, bpp, scene, t);
            return;
        }
        const uint8_t mode = g.failsafe_mode == config::kFailsafeScene ? config::kFailsafeBlackout
                                                                       : g.failsafe_mode;
        logic::fill_failsafe_pattern(buf, kMaxBytesPerChan, cc.pixel_count, bpp, mode, g.failsafe_r,
                                     g.failsafe_g, g.failsafe_b);
        return;
    }

    logic::decode_pixels(buf, kMaxBytesPerChan, cc,
                         [](uint16_t u) { return universe_front_buffer_for(u); });
}

}  // namespace

bool decode_pixels_for_channel(size_t ch) {
    if (ch >= config::kNumChannels) return false;
    uint8_t* dst = pixel_back_buffer(ch);
    if (!dst) return false;
    const config::ChannelConfig cc = effective_channel(ch);

    // Identify blink: top priority — it answers "which strip is this?".
    if (identify_channel() == static_cast<int>(ch)) {
        const uint8_t bpp  = led::bytes_per_pixel(cc.protocol);
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint8_t lvl  = ((now / 250) & 1) ? 255 : 0;  // 2 Hz blink
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
        g_preview_emit.store(
            static_cast<uint16_t>(phys < kMaxPixelsPerChan ? phys : kMaxPixelsPerChan),
            std::memory_order_relaxed);
        logic::fill_preview_pattern(dst, kMaxBytesPerChan, count, emit,
                                    led::bytes_per_pixel(cc.protocol), gps, ngaps);
        return true;
    }

    // Effects and strobe: 64-bit ms, no wrap in a lifetime. The fade keeps a
    // 32-bit stamp — a difference, so its wrap is harmless.
    const uint64_t t   = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const size_t bytes = static_cast<size_t>(cc.pixel_count) * led::bytes_per_pixel(cc.protocol);
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

    // Show control last: it dims whatever the output renders.
    if (((blackout_effective() >> ch) & 1) || !logic::strobe_lit(t, strobe_effective(ch)))
        std::memset(dst, 0, bytes);
    else
        logic::apply_master(dst, bytes, master_effective(ch));
    return true;
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

int channel_for_universe(uint16_t universe_number) {
    const uint16_t slot = slot_for_universe(universe_number);
    if (slot == logic::kNoSlot || g_slot_to_channel[slot] == kNoChannel) return -1;
    return static_cast<int>(g_slot_to_channel[slot]);
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
    const int ch = channel_for_universe(universe_number);
    if (ch < 0) return;
    // Age the timestamp far into the past: still "was active once" (non-zero),
    // but past any timeout whatever the uptime — ageing it to boot+1 µs left a
    // stream terminated within the first failsafe_timeout_s of uptime ignored.
    if (g_last_activity_us[ch].load(std::memory_order_relaxed) != 0)
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
    const uint64_t bit = 1ull << slot;
    if (g_uni_dirty.fetch_or(bit, std::memory_order_acq_rel) & bit) return;
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
        if (g_slot_to_channel[slot] != channel_index) continue;
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
    note_channel_activity(g_slot_to_channel[slot]);
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
    note_channel_activity(g_slot_to_channel[slot]);
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
    // Nothing arrived since the last swap: keep presenting the current front.
    // Swapping here would re-present the other bank, one source frame behind,
    // which is what made a 10 Hz source flicker between its last two frames.
    // Hold-last-value is the stage-lighting convention, and now it really holds:
    // a channel that has not received an update keeps its previous value because
    // the front bank is left alone, not because the banks take turns.
    if (g_uni_dirty.load(std::memory_order_acquire) == 0 || !g_uni_swap_mux) return;
    xSemaphoreTake(g_uni_swap_mux, portMAX_DELAY);
    g_uni_front.store(back_bank_locked(), std::memory_order_release);
    g_uni_dirty.store(0, std::memory_order_relaxed);
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
