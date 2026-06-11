// Host implementation of config_store: keeps GlobalConfig + 8 ChannelConfig in
// RAM with the same factory defaults as the device (no NVS). Mirrors
// make_default_global / make_default_channel from
// components/config_store/src/config_store.cpp so the emulated UI shows the
// same initial state as a freshly-flashed device.

#include "config_store.h"

#include <cstdio>
#include <cstring>

namespace pixfrog::config {

namespace {

GlobalConfig g_global{};
ChannelConfig g_channels[kNumChannels]{};
Scene g_scenes[kNumScenes]{};
bool g_inited = false;

GlobalConfig make_default_global() {
    GlobalConfig g{};
    g.use_dhcp      = true;
    g.artnet_net    = 0;
    g.artnet_subnet = 0;
    std::strncpy(g.short_name, "pixfrog", kArtnetNameShortMax - 1);
    std::strncpy(g.long_name, "pixfrog LED controller", kArtnetNameLongMax - 1);
    g.artnet_poll_reply_unicast = false;
    g.refresh_rate_hz           = 60;
    g.home_timeout_s            = 30;
    return g;
}

ChannelConfig make_default_channel(size_t idx) {
    ChannelConfig c{};
    c.protocol         = led::Protocol::Off;  // mirror device default: channels start disabled
    c.color_order      = led::ColorOrder::GRB;
    c.universe_start   = static_cast<uint16_t>(1 + idx * 6);
    c.dmx_start        = 1;
    c.pixel_count      = 144;
    c.brightness       = 255;
    c.grouping         = 1;
    c.invert_direction = false;
    c.clock_hz         = 4'000'000;
    return c;
}

void ensure_init() {
    if (g_inited) return;
    g_global = make_default_global();
    for (size_t i = 0; i < kNumChannels; ++i)
        g_channels[i] = make_default_channel(i);
    for (size_t i = 0; i < kNumScenes; ++i) {
        std::snprintf(g_scenes[i].name, kSceneNameMax, "Scene %u", static_cast<unsigned>(i + 1));
        g_scenes[i].channel_mask = 0xFF;
    }
    g_inited = true;
}

}  // namespace

void init() {
    ensure_init();
}

const GlobalConfig& get_global() {
    ensure_init();
    return g_global;
}

const ChannelConfig& get_channel(size_t channel_index) {
    ensure_init();
    if (channel_index >= kNumChannels) channel_index = 0;
    return g_channels[channel_index];
}

bool set_global(const GlobalConfig& cfg) {
    ensure_init();
    g_global = cfg;
    return true;
}

bool set_channel(size_t channel_index, const ChannelConfig& cfg) {
    ensure_init();
    if (channel_index >= kNumChannels) return false;
    g_channels[channel_index] = cfg;
    return true;
}

void reset_to_defaults() {
    g_inited = false;
    ensure_init();
}

bool is_persistence_ok() {
    return true;  // emulator: pretend NVS is healthy
}

const Scene& get_scene(size_t i) {
    ensure_init();
    return g_scenes[i < kNumScenes ? i : 0];
}

bool set_scene(size_t i, const Scene& scene) {
    ensure_init();
    if (i >= kNumScenes) return false;
    g_scenes[i] = scene;
    return true;
}

}  // namespace pixfrog::config
