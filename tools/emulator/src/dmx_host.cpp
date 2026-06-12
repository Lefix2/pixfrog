// Host stub for dmx_manager. The UI only reads telemetry (get_stats,
// is_channel_active, is_channel_capacity_ok) for the HOME dashboard and calls
// mark_*_dirty after edits. The emulator has no DMX engine, so stats default to
// zero and channels report idle/ok. emu_dmx_* lets the agent API inject fake
// telemetry to exercise the HOME screen.

#include "dmx_emu.h"
#include "dmx_manager.h"

namespace pixfrog::dmx {

namespace {
Stats g_stats{};
bool g_active[config::kNumChannels]      = {};
bool g_capacity_ok[config::kNumChannels] = { true, true, true, true, true, true, true, true };
}  // namespace

Stats get_stats() {
    return g_stats;
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
bool is_channel_failsafe(size_t /*channel_index*/) {
    return false;
}
namespace {
int g_scene    = -1;
int g_identify = -1;
}  // namespace
void identify_start(size_t channel_index, uint16_t /*seconds*/) {
    g_identify = static_cast<int>(channel_index);
}
void identify_stop() {
    g_identify = -1;
}
int identify_channel() {
    return g_identify;
}
void scene_start(uint8_t scene_index) {
    g_scene = scene_index;
}
void scene_stop() {
    g_scene = -1;
}
int active_scene() {
    return g_scene;
}
void note_universe_terminated(uint16_t /*universe_number*/) {}

// Pixel-count preview: the emulator has no LED output, but the state is kept
// so the menu's set/clear/update logic can be exercised through the agent API.
namespace {
int g_preview_ch         = -1;
uint16_t g_preview_count = 0;
}  // namespace

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
void emu_set_active(size_t ch, bool on) {
    if (ch < config::kNumChannels) g_active[ch] = on;
}

}  // namespace pixfrog::dmx

// ── Emulator-facing telemetry injection (free functions) ─────────────────────
void emu_dmx_set_stats(uint32_t fps, uint64_t pkts) {
    pixfrog::dmx::emu_set_stats(fps, pkts);
}
void emu_dmx_set_active(int ch, bool on) {
    pixfrog::dmx::emu_set_active(static_cast<size_t>(ch), on);
}
