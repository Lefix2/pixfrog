// Host stub for dmx_manager. The UI only reads telemetry (get_stats,
// is_channel_active, is_channel_capacity_ok) for the HOME dashboard and calls
// mark_*_dirty after edits. The emulator has no DMX engine, so stats default to
// zero and channels report idle/ok. emu_dmx_* lets the agent API inject fake
// telemetry to exercise the HOME screen.

#include "dmx_emu.h"
#include "dmx_manager.h"

#include "../../../components/dmx_manager/src/dmx_logic.h"  // the real layout maths

namespace pixfrog::dmx {

namespace {
Stats g_stats{};
bool g_active[config::kNumChannels]      = {};
bool g_failsafe[config::kNumChannels]    = {};
bool g_capacity_ok[config::kNumChannels] = { true, true, true, true, true, true, true, true };
}  // namespace

Stats get_stats() {
    return g_stats;
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

void set_current_fps(uint32_t fps) {
    g_stats.current_fps = fps;
}

bool is_channel_active(size_t channel_index) {
    return channel_index < config::kNumChannels && g_active[channel_index];
}

bool is_channel_capacity_ok(size_t ch) {
    return ch >= config::kNumChannels || g_capacity_ok[ch];
}

void mark_channel_dirty(size_t /*channel_index*/) {}
void mark_global_dirty() {}

// The emulator doesn't link the encoder, so it can't compute the real budget;
// report the absolute cap and never clamp (capacity is exercised on hardware).
config::ChannelConfig effective_channel(size_t ch) {
    return config::get_channel(ch);
}
uint16_t channel_max_pixels(size_t /*ch*/) {
    return static_cast<uint16_t>(led::kMaxPixelsPerChannel);
}
bool auto_patch_universes(uint16_t /*base*/, uint16_t* next_free) {
    if (next_free) *next_free = 0;
    return true;
}
bool is_channel_failsafe(size_t channel_index) {
    return channel_index < config::kNumChannels && g_failsafe[channel_index];
}
void emu_set_failsafe(size_t ch, bool on) {
    if (ch < config::kNumChannels) g_failsafe[ch] = on;
}
namespace {
int g_scene_out[config::kNumChannels]   = { -1, -1, -1, -1, -1, -1, -1, -1 };
int g_identify                          = -1;
uint16_t g_master[config::kNumChannels] = { kMasterFull, kMasterFull, kMasterFull, kMasterFull,
                                            kMasterFull, kMasterFull, kMasterFull, kMasterFull };
uint8_t g_blackout                      = 0;
uint8_t g_strobe[config::kNumChannels]  = {};
}  // namespace
void identify_start(size_t channel_index, uint8_t /*blinks*/) {
    g_identify = static_cast<int>(channel_index);
}
void identify_stop() {
    g_identify = -1;
}
int identify_channel() {
    return g_identify;
}
// Zones as on the device (no crossfade: the emulator renders no pixels).
void scene_start_on(uint8_t scene_index, uint8_t outputs, int32_t) {
    if (scene_index >= config::num_scenes()) return;
    const uint8_t mask = outputs & config::scene_mask(config::get_scene(scene_index));
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((mask >> o) & 1) g_scene_out[o] = scene_index;
}
void scene_start(uint8_t scene_index) {
    scene_start_on(scene_index, kAllOutputs);
}
void scene_stop_on(uint8_t outputs, int32_t) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) g_scene_out[o] = -1;
}
void scene_stop() {
    scene_stop_on(kAllOutputs);
}
uint8_t scene_outputs(uint8_t index) {
    uint8_t m = 0;
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if (g_scene_out[o] == index) m |= static_cast<uint8_t>(1u << o);
    return m;
}
void scene_stop_scene(uint8_t scene_index) {
    scene_stop_on(scene_outputs(scene_index));
}
int scene_on_output(size_t ch) {
    return ch < config::kNumChannels ? g_scene_out[ch] : -1;
}
void scene_list_edited(config::SceneEdit op, size_t a, size_t b) {
    for (auto& sc : g_scene_out)
        sc = config::remap_scene_index(sc, op, a, b);
}
int active_scene() {
    for (int sc : g_scene_out)
        if (sc >= 0) return sc;
    return -1;
}
uint32_t scene_fade_ms() {
    return config::get_global().scene_fade_ms;
}

void master_set(uint8_t outputs, uint16_t level) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) g_master[o] = level;
}
uint16_t master_local(size_t ch) {
    return ch < config::kNumChannels ? g_master[ch] : kMasterFull;
}
uint16_t master_effective(size_t ch) {
    return master_local(ch);
}
void blackout_set(uint8_t outputs, bool on) {
    g_blackout = on ? (g_blackout | outputs) : (g_blackout & static_cast<uint8_t>(~outputs));
}
void blackout_toggle(uint8_t outputs) {
    blackout_set(outputs, (g_blackout & outputs) != outputs);
}
uint8_t blackout_local() {
    return g_blackout;
}
uint8_t blackout_effective() {
    return g_blackout;
}
void strobe_set(uint8_t outputs, uint8_t hz10) {
    for (size_t o = 0; o < config::kNumChannels; ++o)
        if ((outputs >> o) & 1) g_strobe[o] = hz10;
}
uint8_t strobe_local(size_t ch) {
    return ch < config::kNumChannels ? g_strobe[ch] : 0;
}
uint8_t strobe_effective(size_t ch) {
    return strobe_local(ch);
}
bool control_live() {
    return false;
}
void note_universe_terminated(uint16_t /*universe_number*/) {}

// Pixel-count preview: the emulator has no LED output, but the state is kept
// so the menu's set/clear/update logic can be exercised through the agent API.
namespace {
int g_preview_ch         = -1;
uint16_t g_preview_count = 0;
}  // namespace

void set_preview_gaps(const led::PixelGap*, size_t) {}
void set_pixel_preview(size_t channel_index, uint16_t pixel_count) {
    g_preview_ch    = static_cast<int>(channel_index);
    g_preview_count = pixel_count;
}
void clear_pixel_preview() {
    g_preview_ch    = -1;
    g_preview_count = 0;
}
int pixel_preview_channel() {
    return g_preview_ch;
}
uint16_t pixel_preview_count() {
    return g_preview_count;
}
uint16_t preview_emit_count() {
    return g_preview_count;
}

// FSEQ playback active flag.
namespace {
bool g_fseq_active = false;
}  // namespace

void fseq_set_active(bool active) {
    g_fseq_active = active;
}
bool fseq_is_active() {
    return g_fseq_active;
}

// Emulator setters (same TU, can reach the anonymous-namespace state).
void emu_set_stats(uint32_t fps, uint64_t pkts) {
    g_stats.current_fps       = fps;
    g_stats.artnet_packets_rx = pkts;
}
void emu_set_pkts(uint64_t pkts) {
    g_stats.artnet_packets_rx = pkts;
}
void emu_set_active(size_t ch, bool on) {
    if (ch < config::kNumChannels) g_active[ch] = on;
}

}  // namespace pixfrog::dmx

// ── Emulator-facing telemetry injection (free functions) ─────────────────────
void emu_dmx_set_stats(uint32_t fps, uint64_t pkts) {
    pixfrog::dmx::emu_set_stats(fps, pkts);
}
void emu_dmx_set_pkts(uint64_t pkts) {
    pixfrog::dmx::emu_set_pkts(pkts);
}
void emu_dmx_set_active(int ch, bool on) {
    pixfrog::dmx::emu_set_active(static_cast<size_t>(ch), on);
}
void emu_dmx_set_failsafe(int ch, bool on) {
    pixfrog::dmx::emu_set_failsafe(static_cast<size_t>(ch), on);
}
