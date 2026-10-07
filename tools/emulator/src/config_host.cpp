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
SceneBank g_bank{};
bool g_inited = false;

GlobalConfig make_default_global() {
    GlobalConfig g{};
    g.use_dhcp = true;
    std::strncpy(g.short_name, "pixfrog", kArtnetNameShortMax - 1);
    std::strncpy(g.long_name, "pixfrog LED controller", kArtnetNameLongMax - 1);
    g.artnet_poll_reply_unicast = false;
    g.refresh_rate_hz           = 60;
    g.home_timeout_s            = 30;
    g.tft_brightness            = 100;
    g.tft_idle_dim              = 60;
    g.tft_dim_delay_s           = 30;
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
    c.gamma_x10        = 10;  // linear — mirror device default (config_store.cpp)
    return c;
}

void ensure_init() {
    if (g_inited) return;
    g_global = make_default_global();
    for (size_t i = 0; i < kNumChannels; ++i)
        g_channels[i] = make_default_channel(i);
    g_bank.count = kLegacyNumScenes;
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        char name[kSceneNameMax];
        std::snprintf(name, sizeof(name), "Scene %u", static_cast<unsigned>(i + 1));
        g_bank.scenes[i] = make_scene(name, 0xFF, static_cast<uint8_t>(i));
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
    sanitize_channel(g_channels[channel_index]);  // as the NVS store: normalizes gaps
    return true;
}

namespace {
ControlConfig g_control = default_control();
}  // namespace
const ControlConfig& get_control() {
    return g_control;
}
bool set_control(const ControlConfig& cfg) {
    g_control = cfg;
    sanitize_control(g_control);
    return true;
}

void reset_to_defaults() {
    g_inited = false;
    ensure_init();
}

bool is_persistence_ok() {
    return true;  // emulator: pretend NVS is healthy
}

const ProfileBank& get_profiles() {
    static const ProfileBank kBank = default_profiles();
    return kBank;
}

size_t num_scenes() {
    ensure_init();
    return g_bank.count;
}

const Scene& get_scene(size_t i) {
    static const Scene kBlank{};
    ensure_init();
    return i < g_bank.count ? g_bank.scenes[i] : kBlank;
}

bool set_scene(size_t i, const Scene& scene) {
    ensure_init();
    if (i >= g_bank.count) return false;
    g_bank.scenes[i] = scene;
    sanitize_scene(g_bank.scenes[i]);  // as the NVS store: disjoint parts, empty ones dropped
    return true;
}

int add_scene(const Scene& scene) {
    ensure_init();
    if (g_bank.count >= kMaxScenes) return -1;
    g_bank.scenes[g_bank.count] = scene;
    sanitize_scene(g_bank.scenes[g_bank.count]);
    return g_bank.count++;
}

// The effect bank the scene editor picks from (the menu only shows the
// names). One effect short of the scenes, and the last without a name: the
// last scene's effect is missing, as after a delete from the web.
constexpr size_t kEmuEffects = kLegacyNumScenes - 1;

size_t num_effects() {
    return kEmuEffects;
}

const Effect& get_effect(size_t i) {
    static Effect bank[kEmuEffects];
    static const Effect kBlank{};
    static bool filled = false;
    if (!filled) {
        static const char* const kNames[kEmuEffects] = {
            "Warm white", "Chase", "Rainbow", "Blobs", "Fire", "Twinkle", ""
        };
        for (size_t k = 0; k < kEmuEffects; ++k) {
            std::snprintf(bank[k].name, sizeof(bank[k].name), "%s", kNames[k]);
            bank[k].generator  = static_cast<uint8_t>(k % kSceneFxCount);
            bank[k].num_colors = 1;
        }
        filled = true;
    }
    return i < kEmuEffects ? bank[i] : kBlank;
}

// Three fixture groups for the "Plays on" picker, the last one unnamed.
const GroupsConfig& get_groups() {
    static GroupsConfig groups{};
    if (!groups.count) {
        groups.count = 3;
        std::snprintf(groups.groups[0].name, sizeof(groups.groups[0].name), "Top");
        std::snprintf(groups.groups[1].name, sizeof(groups.groups[1].name), "Centre");
    }
    return groups;
}

bool delete_scene(size_t i) {
    ensure_init();
    if (i >= g_bank.count) return false;
    std::memmove(&g_bank.scenes[i], &g_bank.scenes[i + 1], (g_bank.count - i - 1) * sizeof(Scene));
    --g_bank.count;
    return true;
}

bool move_scene(size_t from, size_t to) {
    ensure_init();
    if (from >= g_bank.count || to >= g_bank.count) return false;
    const Scene moved = g_bank.scenes[from];
    if (from < to)
        std::memmove(&g_bank.scenes[from], &g_bank.scenes[from + 1], (to - from) * sizeof(Scene));
    else if (to < from)
        std::memmove(&g_bank.scenes[to + 1], &g_bank.scenes[to], (from - to) * sizeof(Scene));
    g_bank.scenes[to] = moved;
    return true;
}

bool replace_scenes(const Scene* scenes, size_t count) {
    ensure_init();
    if (count > kMaxScenes) count = kMaxScenes;
    for (size_t i = 0; i < count; ++i)
        g_bank.scenes[i] = scenes[i];
    g_bank.count = static_cast<uint8_t>(count);
    return true;
}

}  // namespace pixfrog::config
