// Host stub for dmx_manager. The UI only reads telemetry (get_stats,
// is_channel_active, is_channel_capacity_ok) for the HOME dashboard and calls
// mark_*_dirty after edits. The emulator has no DMX engine, so stats default to
// zero and channels report idle/ok. emu_dmx_* lets the agent API inject fake
// telemetry to exercise the HOME screen.

#include "dmx_manager.h"
#include "dmx_emu.h"

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

// Emulator setters (same TU, can reach the anonymous-namespace state).
void emu_set_stats(uint32_t fps, uint64_t pkts) {
    g_stats.current_fps     = fps;
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
