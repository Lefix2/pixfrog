// config_store — persistent configuration for pixfrog.
//
// Blobs, each its own NVS key (a grown struct loads zero-filled):
//   GlobalConfig   : network, Art-Net identity, refresh, services, display…
//   ChannelConfig  : per-LED-channel settings (×8)
//   SceneBank      : the standalone scenes
//   ControlConfig  : the DMX control universe
//   FseqPlaylist   : the FSEQ playlist
//
// init() loads everything into a RAM cache. get_*() return references into
// that cache (no NVS access, safe from the render path); set_*() update the
// cache then write the blob through.
//
// Concurrency: every setter (and every scene-list edit) takes one recursive
// config lock, so two writers never interleave inside a struct. A caller's
// read-modify-write (get → change a field → set) wraps itself in a
// ScopedLock so another task's write cannot land in between and be lost. The
// render task copies the scenes it draws with copy_scene() (under the lock),
// never through a reference a list edit could memmove under it.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <stdint.h>

#include "led_protocols.h"

namespace pixfrog::config {

constexpr size_t kNumChannels = 8;

constexpr size_t kArtnetNameShortMax = 18;
constexpr size_t kArtnetNameLongMax  = 64;

// ────────────────────────────────────────────────────────────────────────────
// Network + ArtNet identity
// ────────────────────────────────────────────────────────────────────────────

struct GlobalConfig {
    // Network
    bool use_dhcp;
    uint32_t static_ip;  // host-order; 0 if use_dhcp
    uint32_t static_mask;
    uint32_t static_gateway;

    // ArtNet identity
    uint8_t artnet_net;     // 0..127
    uint8_t artnet_subnet;  // 0..15
    char short_name[kArtnetNameShortMax];
    char long_name[kArtnetNameLongMax];
    bool artnet_poll_reply_unicast;  // false=broadcast (default), true=unicast to poller

    // System
    uint8_t refresh_rate_hz;  // kMinRefreshHz..kMaxRefreshHz
    uint16_t home_timeout_s;  // 30 by default

    // Web UI — opt-in; no TCP socket opened when false (default)
    bool web_enabled;

    // sACN (E1.31) receiver — opt-in; no UDP socket opened when false (default)
    bool sacn_enabled;

    // Web UI admin password — salted SHA-256, never stored in clear.
    // hash all-zero = no password set = auth disabled (the default).
    uint8_t web_auth_salt[8];
    uint8_t web_auth_hash[32];  // see web_auth_kdf

    // Signal-loss failsafe — global setting, triggered per channel when no
    // packet touched any of its universes for failsafe_timeout_s. Zero-fill
    // migration = Hold + disabled = today's behaviour.
    uint8_t failsafe_mode;        // 0=hold last look, 1=blackout, 2=solid colour, 3=scene
    uint16_t failsafe_timeout_s;  // 0 = failsafe disabled
    uint8_t failsafe_r;           // solid-colour fill (mode 2)
    uint8_t failsafe_g;
    uint8_t failsafe_b;

    // Standalone scenes hooks (zero-fill migration = none/0).
    // Both follow their scene when the list is reordered (remap_scene_index).
    uint8_t boot_scene;      // 0 = none, N = play scene N-1 at boot
    uint8_t failsafe_scene;  // scene index used by failsafe mode 3

    // 2-source merge policy when two senders feed one universe. Zero-fill
    // migration = HTP, the Art-Net default. Settable via ArtAddress too.
    uint8_t merge_mode;  // 0 = HTP (per-slot max), 1 = LTP (last frame wins)

    // FPP MultiSync remote — opt-in; when true the box follows an FPP/xSchedule
    // master (UDP 32320): start/stop/seek of local FSEQ files. Zero-fill
    // migration = false (no socket opened).
    bool fpp_remote;

    // Web UI display language. Zero-fill migration = 0 = English (the default).
    uint8_t language;  // 0 = English, 1 = French

    // TFT backlight (ignored on OLED builds). All three zero-fill to the
    // behaviour that predates dimming: full brightness, never dimmed.
    uint8_t tft_brightness;    // 10..100 % of full backlight; 0 = unset = 100
    uint8_t tft_idle_dim;      // % dimmer once idle; 0 = never dim
    uint16_t tft_dim_delay_s;  // inactivity before dimming; 0 = never dim

    // Crossfade when a scene starts, stops or replaces another on an output.
    // Zero-fill migration = 0 = instant, the behaviour before fades.
    uint16_t scene_fade_ms;  // 0..kMaxSceneFadeMs

    // Address taken in DHCP mode when no DHCP server answers. Zero-fill
    // migration = link-local, the behaviour it shipped with.
    uint8_t ip_fallback;  // kIpFallback*

    // How web_auth_hash was derived. Zero-fill migration = the original
    // single-round SHA-256(salt || password), re-hashed with the KDF on the
    // next successful login or set.
    uint8_t web_auth_kdf;  // kWebAuthSha256 / kWebAuthPbkdf2

    // This box answers pixfrog.local (the multi-box hub) ahead of the others;
    // without one marked, the lowest MAC does. Zero-fill migration = 0 = no.
    uint8_t hub_preferred;

    // Universe of an FSEQ sequence's first byte (xLights' "start universe").
    // Zero-fill migration = 0 = unset = 1, the xLights default.
    uint16_t fseq_universe;

    // On-board speaker (boards with the R52 mod): knob ticks and the test
    // sound. Zero-fill migration = 0 = off, the default.
    uint8_t speaker_volume;  // 0 = off, 1..100 %
};

constexpr uint8_t kWebAuthSha256 = 0;  // legacy: SHA-256(salt || password)
constexpr uint8_t kWebAuthPbkdf2 = 1;  // PBKDF2-HMAC-SHA256, kWebAuthIterations
// Enough to make an offline guess (from a leaked NVS image or core dump) cost
// thousands of hashes, little enough for a login on the P4 to stay quick.
constexpr uint32_t kWebAuthIterations = 4096;

// DHCP mode without a DHCP server:
//  link-local — 169.254.x.x (RFC 3927, lwIP AutoIP): what a laptop without
//               DHCP also takes, so the two meet with no setup; DHCP keeps
//               being retried and a lease replaces it.
//  artnet     — 2.x.y.z/8 from the MAC (the Art-Net convention many desks
//               default to); DHCP is retried at the next link-up or reboot.
constexpr uint8_t kIpFallbackLinkLocal = 0;
constexpr uint8_t kIpFallbackArtnet    = 1;
inline const char* ip_fallback_id(uint8_t v) {
    return v == kIpFallbackArtnet ? "artnet" : "linklocal";
}
// -1 when unknown.
inline int ip_fallback_from_id(const char* s) {
    if (std::strcmp(s, "linklocal") == 0) return kIpFallbackLinkLocal;
    if (std::strcmp(s, "artnet") == 0) return kIpFallbackArtnet;
    return -1;
}
// The Art-Net fallback address: 2.<mac3>.<mac4>.<mac5>, host order, /8.
// The two host parts a /8 cannot use (all zeros / all ones) are nudged in.
inline uint32_t artnet_fallback_ip(const uint8_t mac[6]) {
    uint32_t host = (static_cast<uint32_t>(mac[3]) << 16) | (static_cast<uint32_t>(mac[4]) << 8) |
                    mac[5];
    if (host == 0) host = 1;
    if (host == 0xFFFFFF) host = 0xFFFFFE;
    return (2u << 24) | host;
}
constexpr uint32_t kArtnetFallbackMask = 0xFF000000u;
inline bool is_link_local(uint32_t host_order_ip) {
    return (host_order_ip >> 16) == 0xA9FE;  // 169.254/16
}

constexpr uint16_t kMaxSceneFadeMs = 25500;  // what one DMX slot can express (×100 ms)

constexpr uint8_t kLangEnglish = 0;
constexpr uint8_t kLangFrench  = 1;

// Below ~10 % the panel is barely readable, so that is the floor the UI, the
// web form and the console all clamp to — dimming should never cost the screen.
constexpr uint8_t kTftBrightnessMin = 10;

// Universe FSEQ byte 0 lands on (0 = unset = 1).
inline uint16_t fseq_universe(const GlobalConfig& g) {
    return g.fseq_universe ? g.fseq_universe : 1;
}

inline uint8_t speaker_volume_pct(const GlobalConfig& g) {
    return g.speaker_volume > 100 ? 100 : g.speaker_volume;
}

// Effective backlight level, in percent of full brightness. 0 (a config written
// by a pre-dimming firmware, zero-filled on migration) means "unset" = 100 %.
inline uint8_t tft_brightness_pct(const GlobalConfig& g) {
    if (g.tft_brightness == 0) return 100;
    if (g.tft_brightness < kTftBrightnessMin) return kTftBrightnessMin;
    return g.tft_brightness > 100 ? 100 : g.tft_brightness;
}

// Idle attenuation, in percent. 100 = backlight fully off once idle.
inline uint8_t tft_idle_dim_pct(const GlobalConfig& g) {
    return g.tft_idle_dim > 100 ? 100 : g.tft_idle_dim;
}

// Longest inactivity the UI accepts before dimming — an hour is already well
// past any "did it crash?" doubt, and it keeps the menu's step count sane.
constexpr uint16_t kTftDimDelayMaxS = 3600;

inline uint16_t tft_dim_delay_s(const GlobalConfig& g) {
    return g.tft_dim_delay_s > kTftDimDelayMaxS ? kTftDimDelayMaxS : g.tft_dim_delay_s;
}

// Idle dimming is armed only when both halves are set: a delay to wait for and
// an attenuation to apply. Either at 0 keeps the panel at its steady level.
inline bool tft_dim_enabled(const GlobalConfig& g) {
    return tft_dim_delay_s(g) != 0 && tft_idle_dim_pct(g) != 0;
}

// Any integer rate in this range. The pixel budget follows it (the wire, not
// the CPU, is the limit above 60 Hz) — see dmx::logic::max_pixels_for.
constexpr uint8_t kMinRefreshHz     = 20;
constexpr uint8_t kMaxRefreshHz     = 120;
constexpr uint8_t kDefaultRefreshHz = 60;

constexpr uint8_t kFailsafeHold     = 0;
constexpr uint8_t kFailsafeBlackout = 1;
constexpr uint8_t kFailsafeColor    = 2;
constexpr uint8_t kFailsafeScene    = 3;

constexpr uint8_t kMergeHtp = 0;
constexpr uint8_t kMergeLtp = 1;

// ────────────────────────────────────────────────────────────────────────────
// Standalone scenes — parametric effects, no recorded frames
// ────────────────────────────────────────────────────────────────────────────

// A variable-length list: 0..kMaxScenes scenes, created, deleted and
// reordered at run time. Scenes are addressed by list position.
constexpr size_t kMaxScenes    = 30;
constexpr size_t kSceneNameMax = 16;
// Fixed slot count of the v1/v2 layouts, kept for their migration.
constexpr size_t kLegacyNumScenes = 8;

// Effect ids are persisted: append only, never renumber.
constexpr uint8_t kSceneFxSolid    = 0;
constexpr uint8_t kSceneFxChase    = 1;
constexpr uint8_t kSceneFxRainbow  = 2;
constexpr uint8_t kSceneFxBlobs    = 3;
constexpr uint8_t kSceneFxGradient = 4;
constexpr uint8_t kSceneFxFade     = 5;
constexpr uint8_t kSceneFxTwinkle  = 6;
constexpr uint8_t kSceneFxFire     = 7;
constexpr uint8_t kSceneFxScanner  = 8;
constexpr uint8_t kSceneFxWave     = 9;
constexpr uint8_t kSceneFxStripes  = 10;
constexpr uint8_t kSceneFxCount    = 11;

constexpr size_t kSceneColorsMax = 4;

// How a scene's effect spreads over a channel's fixtures (Scene::fixture_mode).
// A channel without fixtures is one strip whatever the mode, so per fixture
// is the default (zero): defining fixtures is enough for the scenes to follow.
constexpr uint8_t kFixtureModeEach   = 0;  // each fixture plays the effect on its own
constexpr uint8_t kFixtureModeStrip  = 1;  // over the whole strip, fixtures ignored
constexpr uint8_t kFixtureModeChain  = 2;  // fixtures chained into one strip, end to end
constexpr uint8_t kFixtureModeMirror = 3;  // chained over the first half, mirrored on the rest
constexpr uint8_t kFixtureModeCount  = 4;
inline const char* fixture_mode_id(uint8_t m) {
    static const char* const kIds[] = { "each", "strip", "chain", "mirror" };
    return m < kFixtureModeCount ? kIds[m] : "each";
}

// Display names, indexed by effect id (fixture profiles, TFT).
inline const char* scene_fx_label(uint8_t fx) {
    static const char* const kLabels[] = { "Solid",    "Chase", "Rainbow", "Blobs",
                                           "Gradient", "Fade",  "Twinkle", "Fire",
                                           "Scanner",  "Wave",  "Stripes" };
    static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == kSceneFxCount, "one label per effect");
    return fx < kSceneFxCount ? kLabels[fx] : "Solid";
}

// The first 25 bytes are the pre-palette layout, unchanged (see
// migrate_scenes_v1): colour 1 stays in r/g/b, colours 2.. are appended.
struct Scene {
    char name[kSceneNameMax];               // null-padded
    uint8_t channel_mask;                   // bit n = channel n participates
    uint8_t effect;                         // kSceneFx*
    uint8_t r, g, b;                        // colour 1
    uint8_t speed;                          // per effect — see fill_scene_pattern
    uint8_t param;                          // per effect; 0 = the effect's default
    uint8_t num_colors;                     // 1..kSceneColorsMax; 0 (pre-palette blob) reads as 1
    uint8_t fixture_mode;                   // kFixtureMode*: how the effect spreads over fixtures
    uint8_t extra[kSceneColorsMax - 1][3];  // colours 2..kSceneColorsMax, RGB
};

constexpr size_t kSceneV1Size = 25;
static_assert(offsetof(Scene, num_colors) == 23, "pre-palette Scene layout moved");
static_assert(sizeof(Scene) == kSceneV1Size + 9, "Scene layout changed");

inline uint8_t scene_num_colors(const Scene& s) {
    if (s.num_colors == 0) return 1;
    return s.num_colors > kSceneColorsMax ? static_cast<uint8_t>(kSceneColorsMax) : s.num_colors;
}

// Colour k (0-based) as RGB; k past the configured count reads as black.
inline void scene_color(const Scene& s, size_t k, uint8_t rgb[3]) {
    if (k == 0) {
        rgb[0] = s.r;
        rgb[1] = s.g;
        rgb[2] = s.b;
    } else if (k < scene_num_colors(s)) {
        rgb[0] = s.extra[k - 1][0];
        rgb[1] = s.extra[k - 1][1];
        rgb[2] = s.extra[k - 1][2];
    } else {
        rgb[0] = rgb[1] = rgb[2] = 0;
    }
}

inline void set_scene_color(Scene& s, size_t k, uint8_t r, uint8_t g, uint8_t b) {
    if (k == 0) {
        s.r = r;
        s.g = g;
        s.b = b;
    } else if (k < kSceneColorsMax) {
        s.extra[k - 1][0] = r;
        s.extra[k - 1][1] = g;
        s.extra[k - 1][2] = b;
    }
}

// v1 stored exactly kLegacyNumScenes × 25-byte records in one blob; the grown
// record no longer lines up with it, so it is re-packed record by record.
// Solid used to ignore speed, which now drives its strobe: an upgraded solid
// scene must stay static. Returns false unless old_size is that exact layout.
inline bool migrate_scenes_v1(const uint8_t* old_data, size_t old_size, Scene* dst) {
    if (old_size != kLegacyNumScenes * kSceneV1Size) return false;
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        std::memset(&dst[i], 0, sizeof(Scene));
        std::memcpy(&dst[i], old_data + i * kSceneV1Size, kSceneV1Size);
        dst[i].name[kSceneNameMax - 1] = '\0';
        dst[i].num_colors              = 1;
        dst[i].fixture_mode            = kFixtureModeEach;
        if (dst[i].effect >= kSceneFxCount) dst[i].effect = kSceneFxSolid;
        if (dst[i].effect == kSceneFxSolid) dst[i].speed = 0;
    }
    return true;
}

// NVS image of the scene list (v3): a count byte, then that many records.
// Scene is byte-aligned, so the struct has no padding and the blob is the
// first 1 + count × sizeof(Scene) bytes of it. v1 (200 B) and v2 (8 fixed
// records, 272 B) sizes can never be 1 + k × 34, so the three never collide.
struct SceneBank {
    uint8_t count;
    Scene scenes[kMaxScenes];
};
static_assert(sizeof(SceneBank) == 1 + kMaxScenes * sizeof(Scene), "SceneBank must be packed");

inline size_t scene_bank_bytes(size_t count) {
    return 1 + count * sizeof(Scene);
}

// Parses a stored scene blob of any known layout into `bank`. Returns false
// for an unknown size or an inconsistent count (caller falls back to defaults).
inline bool load_scene_bank(const uint8_t* blob, size_t size, SceneBank& bank) {
    std::memset(&bank, 0, sizeof(bank));
    if (migrate_scenes_v1(blob, size, bank.scenes)) {
        bank.count = kLegacyNumScenes;
    } else if (size == kLegacyNumScenes * sizeof(Scene)) {
        std::memcpy(bank.scenes, blob, size);
        bank.count = kLegacyNumScenes;
    } else if (size >= 1 && blob[0] <= kMaxScenes && size == scene_bank_bytes(blob[0])) {
        std::memcpy(&bank, blob, size);
    } else {
        return false;
    }
    for (size_t i = 0; i < bank.count; ++i) {
        Scene& sc                  = bank.scenes[i];
        sc.name[kSceneNameMax - 1] = '\0';
        sc.num_colors              = scene_num_colors(sc);
        if (sc.effect >= kSceneFxCount) sc.effect = kSceneFxSolid;
    }
    return true;
}

// Where scene `idx` lands after the list is edited; -1 when it was deleted.
// Keeps boot/failsafe/active references on the same scene, not the same slot.
enum class SceneEdit : uint8_t { Delete, Move };
inline int remap_scene_index(int idx, SceneEdit op, size_t a, size_t b = 0) {
    if (idx < 0) return idx;
    const auto i = static_cast<size_t>(idx);
    if (op == SceneEdit::Delete) {
        if (i == a) return -1;
        return i > a ? idx - 1 : idx;
    }
    if (i == a) return static_cast<int>(b);        // the moved scene
    if (a < b && i > a && i <= b) return idx - 1;  // moved down: gap closes up
    if (b < a && i >= b && i < a) return idx + 1;  // moved up: others shift down
    return idx;
}

// ────────────────────────────────────────────────────────────────────────────
// Per-channel configuration
// ────────────────────────────────────────────────────────────────────────────

// Bit clock a fresh channel starts on, and what a zero (pre-clock NVS blob)
// migrates to. Bounds live in led_protocols: they follow from the frame cap.
constexpr uint32_t kDefaultClockHz = 4'000'000;

// A fixture: a run of physical LEDs the scenes treat as one luminaire (a bar,
// a tube). Physical positions, like the gaps (pixel 0 = first on the wire);
// fixtures never overlap each other, a gap inside one only shortens it.
constexpr size_t kMaxFixtures = 32;
struct Fixture {
    uint16_t pos;  // first physical LED, 0-based
    uint16_t len;  // LEDs (0 = unused slot) | kFixtureReversed
};
// In Fixture::len: the fixture is mounted the other way round — the scenes run
// through it backwards (its LEDs on the wire are untouched). A flag bit, not a
// field, so the NVS layout and the sort keep it for free.
constexpr uint16_t kFixtureReversed = 0x8000;
inline uint16_t fixture_len(const Fixture& f) {
    return f.len & 0x7FFF;
}
inline bool fixture_reversed(const Fixture& f) {
    return (f.len & kFixtureReversed) != 0;
}

// Sorts by position, drops empty / out-of-range / overlapping fixtures (the
// later one of an overlap goes) and packs the rest first. Returns the count.
inline size_t normalize_fixtures(Fixture* f, size_t n) {
    size_t used = 0;
    for (size_t i = 0; i < n; ++i)
        if (fixture_len(f[i]) && f[i].pos < led::kMaxPixelsPerChannel) f[used++] = f[i];
    for (size_t i = 1; i < used; ++i)
        for (size_t j = i; j > 0 && f[j].pos < f[j - 1].pos; --j) {
            const Fixture t = f[j];
            f[j]            = f[j - 1];
            f[j - 1]        = t;
        }
    size_t out = 0;
    for (size_t i = 0; i < used; ++i) {
        if (out && f[i].pos < static_cast<uint32_t>(f[out - 1].pos) + fixture_len(f[out - 1]))
            continue;
        f[out++] = f[i];
    }
    for (size_t i = out; i < n; ++i)
        f[i] = Fixture{ 0, 0 };
    return out;
}

inline size_t fixture_count(const Fixture* f, size_t n) {
    size_t k = 0;
    while (k < n && f[k].len)
        ++k;
    return k;
}

// How a channel's pixels fill its DMX universes (ChannelConfig::packing).
constexpr uint8_t kPackContinuous  = 0;  // byte after byte: a pixel may straddle two universes
constexpr uint8_t kPackWholePixels = 1;  // whole pixels only (170 RGB / 128 RGBW per universe)
constexpr uint8_t kPackPerFixture  = 2;  // each fixture from slot 1 of a new universe, whole pixels
constexpr uint8_t kPackCount       = 3;
inline const char* packing_id(uint8_t p) {
    static const char* const kIds[] = { "continuous", "whole", "fixture" };
    return p < kPackCount ? kIds[p] : "continuous";
}
inline int packing_from_id(const char* s) {
    for (uint8_t p = 0; p < kPackCount; ++p)
        if (std::strcmp(s, packing_id(p)) == 0) return p;
    return -1;
}

struct ChannelConfig {
    led::Protocol protocol;
    led::ColorOrder color_order;
    uint16_t universe_start;  // 1..32767
    uint16_t dmx_start;       // 1..512
    uint16_t pixel_count;     // 1..1024
    uint8_t brightness;       // 0..255
    uint8_t grouping;         // 1..8
    bool invert_direction;
    uint32_t clock_hz;  // only for clocked protocols

    // Gamma correction + white balance, applied at encode time (LED
    // protocols only). Appended for NVS tail migration; 0 = unset.
    uint8_t gamma_x10;  // 10 = linear .. 40 = gamma 4.0; 0 → sanitized to 10
    uint8_t wb_r;       // 255 = unity; 0 → sanitized to 255
    uint8_t wb_g;
    uint8_t wb_b;
    // Dead physical pixels (normalized: sorted, merged, used slots first;
    // zero-fill migration = none). pixel_count counts LIVE pixels.
    led::PixelGap gaps[led::kMaxPixelGaps];
    // Fixtures (normalized; zero-fill migration = none: the scenes then see
    // one strip, as before fixtures existed).
    Fixture fixtures[kMaxFixtures];
    // How the pixels fill the DMX universes (kPack*). Zero-fill migration =
    // continuous, the only layout before this field.
    uint8_t packing;
};

// Zero-filled tails from pre-gamma NVS blobs must read as identity — a wb of
// 0 would silently black a colour out. Called after every NVS load.
inline void sanitize_channel(ChannelConfig& c) {
    // Protocol 9 was DMX512 output, moved to the DMX node firmware: such a
    // channel (or any unknown value) comes back disabled rather than emitting
    // its slot count as LED pixels.
    if (static_cast<uint8_t>(c.protocol) >= static_cast<uint8_t>(led::Protocol::COUNT))
        c.protocol = led::Protocol::Off;
    // A clock under led::kMinClockHz inflates a clocked frame past
    // kMaxSamplesPerFrame, and the output backend then refuses every frame —
    // the channel goes dark. 0 means a pre-clock NVS blob rather than a slow
    // request, so it takes the default; clamping it to the floor would quietly
    // drop a legacy channel from 4 MHz to 500 kHz.
    if (c.clock_hz == 0)
        c.clock_hz = kDefaultClockHz;
    else if (c.clock_hz < led::kMinClockHz)
        c.clock_hz = led::kMinClockHz;
    else if (c.clock_hz > led::kMaxClockHz)
        c.clock_hz = led::kMaxClockHz;
    if (c.gamma_x10 < 10 || c.gamma_x10 > 40) c.gamma_x10 = 10;
    if (c.wb_r == 0) c.wb_r = 255;
    if (c.wb_g == 0) c.wb_g = 255;
    if (c.wb_b == 0) c.wb_b = 255;
    // The dropped RGBWW order (former index 8) survives in old NVS blobs; it was
    // identical to RGBW, so remap it there and keep color_order in range (the
    // name tables are indexed by it).
    if (static_cast<uint8_t>(c.color_order) >= static_cast<uint8_t>(led::ColorOrder::COUNT))
        c.color_order = led::ColorOrder::RGBW;
    for (auto& g : c.gaps)
        if (g.pos >= led::kMaxPixelsPerChannel) g.len = 0;
    led::normalize_gaps(c.gaps, led::kMaxPixelGaps);
    normalize_fixtures(c.fixtures, kMaxFixtures);
    if (c.packing >= kPackCount) c.packing = kPackContinuous;
}

// ────────────────────────────────────────────────────────────────────────────
// API
// ────────────────────────────────────────────────────────────────────────────

// Must be called once before any other API. Initializes NVS partition and
// loads defaults if the namespace is empty.
void init();

// Read-only access to current cached config (returns reference into static storage).
// Holds the config lock (recursive) for a read-modify-write:
//   { config::ScopedLock lock; auto g = config::get_global(); g.x = 1; config::set_global(g); }
class ScopedLock {
  public:
    ScopedLock();
    ~ScopedLock();
    ScopedLock(const ScopedLock&)            = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

const GlobalConfig& get_global();
const ChannelConfig& get_channel(size_t channel_index);

// Mutating helpers — write to NVS and update the cache atomically.
// Return true on success.
bool set_global(const GlobalConfig& cfg);
bool set_channel(size_t channel_index, const ChannelConfig& cfg);

// Scene list (RAM-cached, NVS-persisted like channels). Out-of-range reads
// return a blank (black, no channels) scene rather than aliasing another one.
size_t num_scenes();
const Scene& get_scene(size_t scene_index);
// A consistent copy of a scene, taken under the config lock (render path).
// False, with `out` blanked, past the last scene.
bool copy_scene(size_t scene_index, Scene& out);
bool set_scene(size_t scene_index, const Scene& scene);
// Structural edits also remap boot_scene / failsafe_scene; a deleted
// reference is cleared (boot → none, failsafe scene mode → blackout).
// The playing scene lives in dmx_manager: callers remap it with
// dmx::scene_list_edited(). add_scene returns the new index, -1 when full.
int add_scene(const Scene& scene);
bool delete_scene(size_t scene_index);
bool move_scene(size_t from, size_t to);
// Replaces the whole list (backup restore). count is clamped to kMaxScenes.
bool replace_scenes(const Scene* scenes, size_t count);

// ── Web UI admin password ───────────────────────────────────────────────────
// Empty/null password clears the hash (auth disabled). Setting a password
// generates a fresh random salt and stores PBKDF2-HMAC-SHA256(password, salt,
// kWebAuthIterations). A hash from an older firmware (single SHA-256) still
// checks, and is upgraded on the first successful check.
// All three are ui_task/console-context only (NVS write path).
// Longer than kMaxWebPasswordLen is refused (false, nothing changes): every
// path that sets or checks a password must see the same string.
constexpr size_t kMaxWebPasswordLen = 63;
bool set_web_password(const char* password);
bool web_password_set();
bool check_web_password(const char* password);

// PBKDF2-HMAC-SHA256 with a 32-byte output (one block), exposed for tests.
void pbkdf2_sha256(const uint8_t* password, size_t password_len, const uint8_t* salt,
                   size_t salt_len, uint32_t iterations, uint8_t out[32]);

// ────────────────────────────────────────────────────────────────────────────
// DMX control universe ("personality")
// ────────────────────────────────────────────────────────────────────────────
// A user-composed fixture mode: from `address` in `universe`, each slot takes
// one DMX channel (two for a 16-bit Master) and drives one show function on
// the outputs in its mask. The list order is the channel order.

constexpr size_t kMaxControlSlots = 32;

// Persisted: append only, never renumber.
enum class CtlFn : uint8_t {
    None     = 0,  // spare channel (keeps the layout of a desk profile)
    Master   = 1,  // intensity 0..100 % (16-bit with kCtlFlagFine)
    Blackout = 2,  // >= 128 = outputs dark
    Strobe   = 3,  // 0 = off, 1..255 = 1..25 Hz
    Scene    = 4,  // bands of 8: 0-7 = no scene, 8-15 = scene 1, ...
    Speed    = 5,  // 0 = the scene's own, 1..255 = override
    Param    = 6,  // 0 = the scene's own, 1..255 = override
    Red      = 7,  // colour `index` override (R, G and B all 0 = the scene's own)
    Green    = 8,
    Blue     = 9,
    Effect   = 10,  // 0 = the scene's own, 1..255 spread over the effects
    Fade     = 11,  // scene crossfade, value × 100 ms
    Fseq     = 12,  // bands of 8: 0-7 = stop, 8-15 = file 1, ...
    Count,
};
constexpr uint8_t kCtlFlagFine = 0x01;  // Master only: coarse + fine channel

// Lower-case ids (console, REST, backup) — indexed by CtlFn.
inline const char* ctl_fn_id(uint8_t fn) {
    static const char* const kIds[] = { "none",   "master", "blackout", "strobe", "scene",
                                        "speed",  "param",  "red",      "green",  "blue",
                                        "effect", "fade",   "fseq" };
    static_assert(sizeof(kIds) / sizeof(kIds[0]) == static_cast<size_t>(CtlFn::Count),
                  "one id per control function");
    return fn < static_cast<uint8_t>(CtlFn::Count) ? kIds[fn] : "none";
}
// -1 when unknown.
inline int ctl_fn_from_id(const char* s) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(CtlFn::Count); ++i)
        if (std::strcmp(s, ctl_fn_id(i)) == 0) return i;
    return -1;
}

struct ControlSlot {
    uint8_t fn;     // CtlFn
    uint8_t mask;   // outputs it acts on (bit n = output n); 0 reads as all
    uint8_t index;  // Red/Green/Blue: colour 0..kSceneColorsMax-1
    uint8_t flags;  // kCtlFlag*
};

struct ControlConfig {
    uint8_t enabled;
    uint8_t count;      // slots in use
    uint16_t universe;  // Art-Net port-address / sACN universe
    uint16_t address;   // first DMX channel, 1..512
    uint8_t reserved[2];
    ControlSlot slots[kMaxControlSlots];
};

constexpr uint16_t kDefaultControlUniverse = 100;

inline uint8_t control_slot_width(const ControlSlot& s) {
    return (s.fn == static_cast<uint8_t>(CtlFn::Master) && (s.flags & kCtlFlagFine)) ? 2 : 1;
}

// DMX channels the whole mode occupies.
inline size_t control_footprint(const ControlConfig& c) {
    size_t n = 0;
    for (size_t i = 0; i < c.count && i < kMaxControlSlots; ++i)
        n += control_slot_width(c.slots[i]);
    return n;
}

inline ControlSlot control_slot(CtlFn fn, uint8_t mask = 0xFF, uint8_t index = 0,
                                uint8_t flags = 0) {
    return ControlSlot{ static_cast<uint8_t>(fn), mask, index, flags };
}

// Starting points for the editor. They replace the slot list only.
enum class ControlPreset : uint8_t { Simple, Full };
inline void control_apply_preset(ControlConfig& c, ControlPreset p) {
    std::memset(c.slots, 0, sizeof(c.slots));
    size_t n     = 0;
    c.slots[n++] = control_slot(CtlFn::Master, 0xFF, 0, kCtlFlagFine);
    c.slots[n++] = control_slot(CtlFn::Blackout);
    c.slots[n++] = control_slot(CtlFn::Strobe);
    c.slots[n++] = control_slot(CtlFn::Scene);
    if (p == ControlPreset::Full) {
        c.slots[n++] = control_slot(CtlFn::Speed);
        c.slots[n++] = control_slot(CtlFn::Param);
        c.slots[n++] = control_slot(CtlFn::Effect);
        for (uint8_t k = 0; k < 2; ++k) {
            c.slots[n++] = control_slot(CtlFn::Red, 0xFF, k);
            c.slots[n++] = control_slot(CtlFn::Green, 0xFF, k);
            c.slots[n++] = control_slot(CtlFn::Blue, 0xFF, k);
        }
    }
    c.slots[n++] = control_slot(CtlFn::Fade);
    if (p == ControlPreset::Full) c.slots[n++] = control_slot(CtlFn::Fseq);
    c.count = static_cast<uint8_t>(n);
}

inline ControlConfig default_control() {
    ControlConfig c{};
    c.enabled  = 0;
    c.universe = kDefaultControlUniverse;
    c.address  = 1;
    control_apply_preset(c, ControlPreset::Simple);
    return c;
}

// Keeps every field meaningful: unknown functions become spare channels, a
// fine flag only stays on Master, and the mode is trimmed so it ends inside
// the universe. Called on load and on every set.
inline void sanitize_control(ControlConfig& c) {
    c.enabled = c.enabled ? 1 : 0;
    if (c.universe > 0x7FFF) c.universe = kDefaultControlUniverse;
    if (c.address < 1 || c.address > 512) c.address = 1;
    if (c.count > kMaxControlSlots) c.count = kMaxControlSlots;
    for (size_t i = 0; i < kMaxControlSlots; ++i) {
        ControlSlot& s = c.slots[i];
        if (i >= c.count) {
            s = ControlSlot{};
            continue;
        }
        if (s.fn >= static_cast<uint8_t>(CtlFn::Count)) s.fn = static_cast<uint8_t>(CtlFn::None);
        if (s.mask == 0) s.mask = 0xFF;
        if (s.index >= kSceneColorsMax) s.index = 0;
        if (s.fn != static_cast<uint8_t>(CtlFn::Master))
            s.flags &= static_cast<uint8_t>(~kCtlFlagFine);
        s.flags &= kCtlFlagFine;
    }
    size_t used = 0, keep = 0;
    for (; keep < c.count; ++keep) {
        const size_t w = control_slot_width(c.slots[keep]);
        if (c.address - 1u + used + w > 512) break;
        used += w;
    }
    for (size_t i = keep; i < c.count; ++i)
        c.slots[i] = ControlSlot{};
    c.count = static_cast<uint8_t>(keep);
}

const ControlConfig& get_control();
bool set_control(const ControlConfig& cfg);

// ── FSEQ playlist ───────────────────────────────────────────────────────────
// Files of the SD card played in order, each `repeat` times; `loop` starts
// over after the last one; `autostart` plays it as soon as the card mounts at
// boot. A file to loop at boot = a one-item looping playlist. Own NVS blob,
// absent on an upgrade (= empty, nothing autostarts).
constexpr size_t kPlaylistMax     = 16;
constexpr size_t kPlaylistNameLen = 64;  // = fseq::kMaxNameLen
struct PlaylistItem {
    char name[kPlaylistNameLen];  // file in the card root, NUL-terminated
    uint8_t repeat;               // plays in a row, 1..255
    uint8_t reserved[3];
};
struct FseqPlaylist {
    uint8_t count;  // items in use
    uint8_t loop;
    uint8_t autostart;
    uint8_t reserved;
    PlaylistItem items[kPlaylistMax];
};

// Booleans to 0/1, count capped, repeat at least 1, names terminated, empty
// names dropped (the rest close up), unused items zeroed.
inline void sanitize_playlist(FseqPlaylist& p) {
    p.loop      = p.loop ? 1 : 0;
    p.autostart = p.autostart ? 1 : 0;
    p.reserved  = 0;
    if (p.count > kPlaylistMax) p.count = kPlaylistMax;
    size_t keep = 0;
    for (size_t i = 0; i < p.count; ++i) {
        PlaylistItem it               = p.items[i];
        it.name[kPlaylistNameLen - 1] = '\0';
        if (!it.name[0]) continue;
        if (it.repeat == 0) it.repeat = 1;
        std::memset(it.reserved, 0, sizeof(it.reserved));
        p.items[keep++] = it;
    }
    for (size_t i = keep; i < kPlaylistMax; ++i)
        p.items[i] = PlaylistItem{};
    p.count = static_cast<uint8_t>(keep);
}

const FseqPlaylist& get_playlist();
bool set_playlist(const FseqPlaylist& p);

// ── Fixture groups ──────────────────────────────────────────────────────────
// Named, ordered sets of fixtures taken on any outputs ("Top", "Bottom",
// "Centre", one bar…): where a scene plays. The order is the virtual strip a
// scene runs along (chain / mirror), whatever the wiring. A member is a
// fixture by output and index in that output's strip order — editing an
// output's fixture list can shift what a group points at. Own NVS blob,
// absent on an upgrade (= no groups).
constexpr size_t kMaxGroups       = 16;
constexpr size_t kMaxGroupMembers = 64;
constexpr size_t kGroupNameMax    = 16;
struct FixtureRef {
    uint8_t output;   // 0..kNumChannels-1
    uint8_t fixture;  // 0..kMaxFixtures-1, in the output's strip order
};
struct FixtureGroup {
    char name[kGroupNameMax];  // NUL-terminated
    uint8_t count;             // members in use
    uint8_t reserved[3];
    FixtureRef members[kMaxGroupMembers];
};
struct GroupsConfig {
    uint8_t count;  // groups in use
    uint8_t reserved[3];
    FixtureGroup groups[kMaxGroups];
};

// Counts capped, names terminated, members out of range or repeated dropped
// (the rest close up), unused slots zeroed. A group keeps its place even
// when empty (scenes and the desk refer to groups by index).
inline void sanitize_groups(GroupsConfig& g) {
    if (g.count > kMaxGroups) g.count = kMaxGroups;
    std::memset(g.reserved, 0, sizeof(g.reserved));
    for (size_t i = 0; i < kMaxGroups; ++i) {
        FixtureGroup& fg = g.groups[i];
        if (i >= g.count) {
            fg = FixtureGroup{};
            continue;
        }
        fg.name[kGroupNameMax - 1] = '\0';
        std::memset(fg.reserved, 0, sizeof(fg.reserved));
        if (fg.count > kMaxGroupMembers) fg.count = kMaxGroupMembers;
        size_t keep = 0;
        for (size_t m = 0; m < fg.count; ++m) {
            const FixtureRef r = fg.members[m];
            bool dup           = r.output >= kNumChannels || r.fixture >= kMaxFixtures;
            for (size_t k = 0; k < keep && !dup; ++k)
                dup = fg.members[k].output == r.output && fg.members[k].fixture == r.fixture;
            if (!dup) fg.members[keep++] = r;
        }
        for (size_t m = keep; m < kMaxGroupMembers; ++m)
            fg.members[m] = FixtureRef{};
        fg.count = static_cast<uint8_t>(keep);
    }
}

const GroupsConfig& get_groups();
bool set_groups(const GroupsConfig& g);

// Restore defaults (factory reset). Does NOT reboot.
void reset_to_defaults();

// True if config_store successfully opened the NVS namespace at boot and
// is able to persist subsequent set_global/set_channel writes. False if
// NVS was corrupt beyond recovery and we're running on hard-coded RAM
// defaults — in that mode all setters update the cache but return false
// because nothing is written to flash.
bool is_persistence_ok();

// ── OTA rollback record ─────────────────────────────────────────────────────
// The last firmware image the bootloader rejected (a new OTA image that reset
// before confirming itself). Kept in NVS so it outlives the boot that noticed
// it — and a factory reset: it is diagnostics, not configuration — until the
// user acknowledges it.
struct RollbackRecord {
    char rejected_version[32];  // esp_app_desc_t::version of the rejected image
    char rejected_slot[16];     // its OTA partition label
    char running_version[32];   // what booted instead
    uint8_t rejected_sha[8];    // elf SHA-256 prefix: identifies the image
    uint8_t reset_reason;       // esp_reset_reason_t of the boot that rolled back
    uint8_t acknowledged;       // 1 once seen by the user (web / console)
};

// False when no rollback was ever recorded.
bool get_rollback(RollbackRecord& out);
bool set_rollback(const RollbackRecord& rec);

}  // namespace pixfrog::config
