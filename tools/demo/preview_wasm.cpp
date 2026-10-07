// The firmware's effect engine for the web UI demo, as WebAssembly: the
// demo box draws an effect — and a scene over an output's fixtures — with
// the very code the board runs (dmx_logic.h), so the preview is the look.
//
//     tools/demo/build_wasm.sh        # em++ → tools/demo/preview.wasm
//
// A C ABI over static buffers (no allocator, no libc beyond memcpy/memset):
// the page fills the effect and the fixtures through the setters, then asks
// for frames. Everything is bounds-checked here, nothing trusts the caller.
#include <cstdint>
#include <cstring>

#include "config_store.h"
#include "dmx_logic.h"

namespace {
using namespace pixfrog;

config::Effect g_effect{};
config::ChannelConfig g_chan{};
uint8_t g_out[led::kMaxPixelsPerChannel * 3];
char g_str[32];
}  // namespace

extern "C" {

#define PF_EXPORT __attribute__((used, visibility("default")))

// Scratch strings go here (ids of a waveform or a fixture mode), NUL-ended.
PF_EXPORT char* pf_str() {
    return g_str;
}
PF_EXPORT uint8_t* pf_out() {
    return g_out;
}
PF_EXPORT int pf_out_capacity() {
    return static_cast<int>(sizeof(g_out));
}
PF_EXPORT int pf_wave_from_id() {
    g_str[sizeof(g_str) - 1] = 0;
    return config::phaser_wave_from_id(g_str);
}
PF_EXPORT int pf_fixture_mode_from_id() {
    g_str[sizeof(g_str) - 1] = 0;
    for (uint8_t m = 0; m < config::kFixtureModeCount; ++m)
        if (std::strcmp(g_str, config::fixture_mode_id(m)) == 0) return m;
    return -1;
}

// The effect under preview, field by field (the API's numbers; colours as
// 0xRRGGBB, up to kSceneColorsMax).
PF_EXPORT void pf_effect_begin(int generator, int speed, int param) {
    g_effect           = config::Effect{};
    g_effect.generator = static_cast<uint8_t>(generator < 0 ? 0 : generator);
    g_effect.speed     = static_cast<uint8_t>(speed);
    g_effect.param     = static_cast<uint8_t>(param);
}
PF_EXPORT void pf_effect_color(int index, int rgb) {
    if (index < 0 || index >= static_cast<int>(config::kSceneColorsMax)) return;
    g_effect.colors[index][0] = static_cast<uint8_t>(rgb >> 16);
    g_effect.colors[index][1] = static_cast<uint8_t>(rgb >> 8);
    g_effect.colors[index][2] = static_cast<uint8_t>(rgb);
    if (index + 1 > g_effect.num_colors) g_effect.num_colors = static_cast<uint8_t>(index + 1);
}
PF_EXPORT void pf_effect_phaser(int wave, int rate, int spread, int width, int low, int attack,
                                int decay, int reverse) {
    g_effect.ph_wave   = static_cast<uint8_t>(wave < 0 ? 0 : wave);
    g_effect.ph_rate   = static_cast<uint8_t>(rate);
    g_effect.ph_spread = static_cast<uint8_t>(spread);
    g_effect.ph_width  = static_cast<uint8_t>(width);
    g_effect.ph_low    = static_cast<uint8_t>(low);
    g_effect.ph_attack = static_cast<uint8_t>(attack);
    g_effect.ph_decay  = static_cast<uint8_t>(decay);
    g_effect.flags     = static_cast<uint8_t>((g_effect.flags & ~config::kEffectPhaserReverse) |
                                              (reverse ? config::kEffectPhaserReverse : 0));
}
PF_EXPORT void pf_effect_matricks(int invert, int block, int groups, int wings) {
    g_effect.flags  = static_cast<uint8_t>((g_effect.flags & ~config::kEffectDimmerInvert) |
                                           (invert ? config::kEffectDimmerInvert : 0));
    g_effect.block  = static_cast<uint8_t>(block);
    g_effect.groups = static_cast<uint8_t>(groups);
    g_effect.wings  = static_cast<uint8_t>(wings);
}

// The effect on a plain run of `pixels` RGB pixels at `t_ms`, into pf_out():
// what POST /api/effect/preview draws. Returns the pixels drawn.
PF_EXPORT int pf_render(int pixels, double t_ms) {
    if (pixels < 1) return 0;
    if (pixels > static_cast<int>(led::kMaxPixelsPerChannel)) pixels = led::kMaxPixelsPerChannel;
    dmx::logic::fill_effect_run(g_out, sizeof(g_out), static_cast<uint16_t>(pixels), 3, g_effect,
                                static_cast<uint64_t>(t_ms < 0 ? 0 : t_ms));
    return pixels;
}

// An output's strip for a scene: `pixels` LEDs and its fixtures, listed one
// by one before rendering, played in `mode` (kFixtureMode*), reversed or
// not — what the box shows on that output.
PF_EXPORT void pf_strip_begin(int pixels) {
    g_chan             = config::ChannelConfig{};
    g_chan.protocol    = led::Protocol::WS2815;
    g_chan.pixel_count = static_cast<uint16_t>(
        pixels < 1                                             ? 1
        : pixels > static_cast<int>(led::kMaxPixelsPerChannel) ? led::kMaxPixelsPerChannel
                                                               : pixels);
    g_chan.grouping = 1;
}
PF_EXPORT void pf_strip_fixture(int index, int first, int count, int reversed) {
    if (index < 0 || index >= static_cast<int>(config::kMaxFixtures) || count < 1) return;
    g_chan.fixtures[index] = config::make_fixture(static_cast<uint16_t>(first < 0 ? 0 : first),
                                                  static_cast<uint16_t>(count), reversed != 0);
}
PF_EXPORT int pf_render_strip(int mode, int reverse, double t_ms) {
    config::normalize_fixtures(g_chan.fixtures, config::kMaxFixtures);
    const uint8_t m = static_cast<uint8_t>((mode < 0 ? 0 : mode) & 0x03) |
                      (reverse ? config::kSceneReverseBit : 0);
    std::memset(g_out, 0, static_cast<size_t>(g_chan.pixel_count) * 3);
    dmx::logic::fill_effect_on_channel(g_out, sizeof(g_out), g_chan, 3, g_effect, m,
                                       static_cast<uint64_t>(t_ms < 0 ? 0 : t_ms));
    return g_chan.pixel_count;
}

}  // extern "C"
