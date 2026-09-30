// Host-side unit tests for config_store struct layout and NVS blob migration.
//
// What these tests verify:
//   1. GlobalConfig / ChannelConfig struct layout is stable (regression guard
//      against accidental field reordering that would silently corrupt NVS).
//   2. The forward-migration logic (blob smaller than current struct → zero-fill
//      the new tail) correctly defaults new fields and preserves old ones.
//   3. The downgrade-detection path (stored blob larger than current struct)
//      returns false, preventing corrupt reads.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config_store.h"

using namespace pixfrog::config;
using namespace pixfrog::led;

static int g_pass = 0, g_fail = 0;

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long va = static_cast<long long>(a);                                                  \
        long long vb = static_cast<long long>(b);                                                  \
        if (va == vb) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__, __LINE__, #a,  \
                         #b, va, vb);                                                              \
        }                                                                                          \
    } while (0)

#define EXPECT_TRUE(a)                                                                             \
    do {                                                                                           \
        if (a) {                                                                                   \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #a);                      \
        }                                                                                          \
    } while (0)

// ── Migration simulation ─────────────────────────────────────────────────────
//
// Mirrors the logic in config_store.cpp::nvs_load_blob:
//   memset(dst, 0, size); then copy the first min(old_size, size) bytes.
// Returns false when old_size > size (downgrade scenario — blob is larger
// than the struct the firmware knows about).

template <typename T> bool migrate_blob(const void* old_data, size_t old_size, T& dst) {
    if (old_size > sizeof(T)) return false;
    std::memset(&dst, 0, sizeof(T));
    std::memcpy(&dst, old_data, old_size);
    return true;
}

// ── Layout invariants ────────────────────────────────────────────────────────

// Pre-web_enabled snapshot of GlobalConfig (matches the struct at the commit
// before feat/web-config). Used to simulate a firmware upgrade scenario.
// Fields must stay in the same order and with the same types as the old struct.
struct GlobalConfigPreWeb {
    bool use_dhcp;
    uint32_t static_ip;
    uint32_t static_mask;
    uint32_t static_gateway;
    uint8_t artnet_net;
    uint8_t artnet_subnet;
    char short_name[18];
    char long_name[64];
    bool artnet_poll_reply_unicast;
    uint8_t refresh_rate_hz;
    uint16_t home_timeout_s;
};

// The new struct must be strictly larger (web_enabled was appended).
static_assert(sizeof(GlobalConfig) > sizeof(GlobalConfigPreWeb),
              "GlobalConfig must be larger than pre-web snapshot");

// web_enabled must live after home_timeout_s.
static_assert(offsetof(GlobalConfig, web_enabled) >=
                  offsetof(GlobalConfig, home_timeout_s) + sizeof(uint16_t),
              "web_enabled must follow home_timeout_s in struct");

// ── Migration tests ──────────────────────────────────────────────────────────

static void test_migrate_from_pre_web_fields_preserved() {
    // Build a realistic old blob with known non-default values.
    GlobalConfigPreWeb old{};
    old.use_dhcp       = false;
    old.static_ip      = 0xC0A801C8u;  // 192.168.1.200
    old.static_mask    = 0xFFFFFF00u;
    old.static_gateway = 0xC0A80101u;  // 192.168.1.1
    old.artnet_net     = 3;
    old.artnet_subnet  = 7;
    std::strncpy(old.short_name, "myfrog", sizeof(old.short_name) - 1);
    std::strncpy(old.long_name, "My pixfrog controller", sizeof(old.long_name) - 1);
    old.artnet_poll_reply_unicast = true;
    old.refresh_rate_hz           = 30;
    old.home_timeout_s            = 120;

    GlobalConfig loaded{};
    EXPECT_TRUE(migrate_blob(&old, sizeof(old), loaded));

    EXPECT_EQ(loaded.use_dhcp, false);
    EXPECT_EQ(loaded.static_ip, 0xC0A801C8u);
    EXPECT_EQ(loaded.static_mask, 0xFFFFFF00u);
    EXPECT_EQ(loaded.static_gateway, 0xC0A80101u);
    EXPECT_EQ(loaded.artnet_net, 3);
    EXPECT_EQ(loaded.artnet_subnet, 7);
    EXPECT_TRUE(std::strcmp(loaded.short_name, "myfrog") == 0);
    EXPECT_TRUE(std::strcmp(loaded.long_name, "My pixfrog controller") == 0);
    EXPECT_EQ(loaded.artnet_poll_reply_unicast, true);
    EXPECT_EQ(loaded.refresh_rate_hz, 30);
    EXPECT_EQ(loaded.home_timeout_s, 120);
}

static void test_migrate_from_pre_web_new_field_is_false() {
    GlobalConfigPreWeb old{};
    old.use_dhcp = true;

    GlobalConfig loaded{};
    EXPECT_TRUE(migrate_blob(&old, sizeof(old), loaded));

    // web_enabled must default to false after migration from an old blob.
    EXPECT_EQ(loaded.web_enabled, false);
}

// Backlight fields appended after `language`: a config written by a
// pre-dimming firmware zero-fills them, and the accessors must read that back
// as "full brightness, never dimmed" — the behaviour that firmware had.
static void test_migrate_backlight_defaults_to_full_brightness() {
    GlobalConfigPreWeb old{};
    old.use_dhcp = true;

    GlobalConfig loaded{};
    EXPECT_TRUE(migrate_blob(&old, sizeof(old), loaded));

    EXPECT_EQ(loaded.tft_brightness, 0);  // unset
    EXPECT_EQ(loaded.tft_idle_dim, 0);
    EXPECT_EQ(loaded.tft_dim_delay_s, 0);
    EXPECT_EQ(tft_brightness_pct(loaded), 100);
    EXPECT_EQ(tft_idle_dim_pct(loaded), 0);
    EXPECT_EQ(tft_dim_delay_s(loaded), 0);
    EXPECT_TRUE(!tft_dim_enabled(loaded));  // no delay, no attenuation

    // An explicitly stored level is returned as-is; out-of-range values clamp.
    loaded.tft_brightness = 45;
    EXPECT_EQ(tft_brightness_pct(loaded), 45);
    loaded.tft_brightness = 3;
    EXPECT_EQ(tft_brightness_pct(loaded), kTftBrightnessMin);
    loaded.tft_brightness = 200;
    EXPECT_EQ(tft_brightness_pct(loaded), 100);
    loaded.tft_idle_dim = 200;
    EXPECT_EQ(tft_idle_dim_pct(loaded), 100);
    loaded.tft_dim_delay_s = 9'000;
    EXPECT_EQ(tft_dim_delay_s(loaded), kTftDimDelayMaxS);

    // Dimming needs both halves — either one at 0 leaves the panel alone.
    loaded.tft_idle_dim    = 60;
    loaded.tft_dim_delay_s = 30;
    EXPECT_TRUE(tft_dim_enabled(loaded));
    loaded.tft_dim_delay_s = 0;
    EXPECT_TRUE(!tft_dim_enabled(loaded));
    loaded.tft_dim_delay_s = 30;
    loaded.tft_idle_dim    = 0;
    EXPECT_TRUE(!tft_dim_enabled(loaded));
}

static void test_migrate_exact_size_ok() {
    // Exact-size load (no migration needed): all fields should come through.
    GlobalConfig src{};
    src.use_dhcp        = false;
    src.static_ip       = 0x0A000001u;
    src.artnet_net      = 5;
    src.refresh_rate_hz = 60;
    src.web_enabled     = true;

    GlobalConfig loaded{};
    EXPECT_TRUE(migrate_blob(&src, sizeof(src), loaded));

    EXPECT_EQ(loaded.use_dhcp, false);
    EXPECT_EQ(loaded.static_ip, 0x0A000001u);
    EXPECT_EQ(loaded.artnet_net, 5);
    EXPECT_EQ(loaded.refresh_rate_hz, 60);
    EXPECT_EQ(loaded.web_enabled, true);
}

static void test_migrate_rejects_downgrade() {
    // Simulate a downgrade: the stored blob is LARGER than the current struct.
    // migrate_blob must return false without touching dst.
    const size_t oversized = sizeof(GlobalConfig) + 16;
    uint8_t big_blob[oversized];
    std::memset(big_blob, 0xAB, oversized);

    GlobalConfig loaded{};
    std::memset(&loaded, 0xCC, sizeof(loaded));  // sentinel pattern
    EXPECT_TRUE(!migrate_blob(big_blob, oversized, loaded));

    // dst must be untouched when migration returns false.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&loaded);
    bool untouched   = true;
    for (size_t i = 0; i < sizeof(loaded); ++i)
        if (p[i] != 0xCC) {
            untouched = false;
            break;
        }
    EXPECT_TRUE(untouched);
}

static void test_migrate_zero_size_blob_all_zero() {
    // Migrating from a zero-length blob (e.g. first boot after NVS erase)
    // must give a fully-zeroed struct. Default init should then be applied
    // separately (tested here as: are all bytes zero after migration?).
    GlobalConfig loaded{};
    std::memset(&loaded, 0xFF, sizeof(loaded));

    uint8_t empty[1] = { 0 };
    EXPECT_TRUE(migrate_blob(empty, 0, loaded));

    const uint8_t* p = reinterpret_cast<const uint8_t*>(&loaded);
    bool all_zero    = true;
    for (size_t i = 0; i < sizeof(loaded); ++i)
        if (p[i] != 0) {
            all_zero = false;
            break;
        }
    EXPECT_TRUE(all_zero);
}

// ── ChannelConfig layout guard ───────────────────────────────────────────────

// Pre-gamma snapshot of ChannelConfig (before gamma_x10/wb_* were appended).
// Used to prove the zero-fill migration + sanitize lands on identity.
struct ChannelConfigPreGamma {
    Protocol protocol;
    ColorOrder color_order;
    uint16_t universe_start;
    uint16_t dmx_start;
    uint16_t pixel_count;
    uint8_t brightness;
    uint8_t grouping;
    bool invert_direction;
    uint32_t clock_hz;
};

static_assert(sizeof(ChannelConfig) > sizeof(ChannelConfigPreGamma),
              "ChannelConfig must be larger than the pre-gamma snapshot");

static_assert(offsetof(ChannelConfig, protocol) == offsetof(ChannelConfigPreGamma, protocol));
static_assert(offsetof(ChannelConfig, color_order) == offsetof(ChannelConfigPreGamma, color_order));
static_assert(offsetof(ChannelConfig, universe_start) ==
              offsetof(ChannelConfigPreGamma, universe_start));
static_assert(offsetof(ChannelConfig, clock_hz) == offsetof(ChannelConfigPreGamma, clock_hz));

static void test_channel_migration_sanitizes_to_identity() {
    ChannelConfigPreGamma old{};
    old.protocol    = Protocol::WS2815;
    old.pixel_count = 144;
    old.brightness  = 200;

    ChannelConfig loaded{};
    EXPECT_TRUE(migrate_blob(&old, sizeof(old), loaded));
    // Zero-filled tail before sanitize: wb 0 would black the channel out.
    EXPECT_EQ(loaded.gamma_x10, 0);
    sanitize_channel(loaded);
    EXPECT_EQ(loaded.gamma_x10, 10);
    EXPECT_EQ(loaded.wb_r, 255);
    EXPECT_EQ(loaded.wb_g, 255);
    EXPECT_EQ(loaded.wb_b, 255);
    EXPECT_EQ(loaded.brightness, 200);  // old fields preserved
    EXPECT_EQ(loaded.pixel_count, 144);
}

// A clock below kMinClockHz inflates a clocked frame past
// kMaxSamplesPerFrame; the output backend then refuses every frame and the
// channel goes dark. sanitize_channel is the last line of defence for values
// that reached NVS before the write paths were bounded.
static void test_sanitize_clamps_clock_hz() {
    ChannelConfig c{};

    // 0 is a pre-clock blob, not a slow request: it takes the default, not the
    // floor, or a legacy channel silently drops from 4 MHz to 500 kHz.
    c.clock_hz = 0;
    sanitize_channel(c);
    EXPECT_EQ(c.clock_hz, kDefaultClockHz);

    // The value that used to reach the encoder from the console and web API.
    c.clock_hz = 100'000;
    sanitize_channel(c);
    EXPECT_EQ(c.clock_hz, kMinClockHz);

    c.clock_hz = kMaxClockHz + 1;
    sanitize_channel(c);
    EXPECT_EQ(c.clock_hz, kMaxClockHz);

    // In-range values are left alone.
    c.clock_hz = 4'000'000;
    sanitize_channel(c);
    EXPECT_EQ(c.clock_hz, 4'000'000u);
}

// ── main ──────────────────────────────────────────────────────────────────────

// Pre-palette scene record: the scene array was stored as one NVS blob of
// kLegacyNumScenes of these, so a grown record needs re-packing, not a tail fill.
struct SceneV1 {
    char name[kSceneNameMax];
    uint8_t channel_mask;
    uint8_t effect;
    uint8_t r, g, b;
    uint8_t speed;
    uint8_t param;
    uint8_t reserved[2];
};
static_assert(sizeof(SceneV1) == kSceneV1Size, "v1 scene record is 25 bytes");
static_assert(offsetof(Scene, param) == offsetof(SceneV1, param), "v1 prefix must not move");

static void test_scene_v1_migration() {
    SceneV1 old[kLegacyNumScenes]{};
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        std::snprintf(old[i].name, kSceneNameMax, "Old %u", static_cast<unsigned>(i));
        old[i].channel_mask = static_cast<uint8_t>(1u << i);
        old[i].r            = static_cast<uint8_t>(10 + i);
        old[i].speed        = 77;
        old[i].param        = 5;
    }
    old[1].effect = kSceneFxChase;
    old[2].effect = kSceneFxRainbow;

    Scene loaded[kLegacyNumScenes];
    EXPECT_TRUE(migrate_scenes_v1(reinterpret_cast<const uint8_t*>(old), sizeof(old), loaded));
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        EXPECT_TRUE(std::strcmp(loaded[i].name, old[i].name) == 0);
        EXPECT_EQ(loaded[i].channel_mask, old[i].channel_mask);
        EXPECT_EQ(loaded[i].r, 10 + i);
        EXPECT_EQ(loaded[i].param, 5);
        EXPECT_EQ(scene_num_colors(loaded[i]), 1);
        uint8_t rgb[3];
        scene_color(loaded[i], 1, rgb);
        EXPECT_EQ(rgb[0] | rgb[1] | rgb[2], 0);
    }
    // Solid ignored speed before; it is the strobe rate now — must not start flashing.
    EXPECT_EQ(loaded[0].speed, 0);
    EXPECT_EQ(loaded[1].effect, kSceneFxChase);
    EXPECT_EQ(loaded[1].speed, 77);
    EXPECT_EQ(loaded[2].speed, 77);

    // Any other size is not the v1 array.
    EXPECT_TRUE(!migrate_scenes_v1(reinterpret_cast<const uint8_t*>(old), sizeof(old) - 1, loaded));
}

static void test_scene_colour_accessors() {
    Scene s{};
    EXPECT_EQ(scene_num_colors(s), 1);  // zero reads as one colour
    set_scene_color(s, 0, 1, 2, 3);
    set_scene_color(s, 3, 7, 8, 9);
    s.num_colors = 4;
    uint8_t rgb[3];
    scene_color(s, 0, rgb);
    EXPECT_EQ(rgb[2], 3);
    scene_color(s, 3, rgb);
    EXPECT_EQ(rgb[0], 7);
    s.num_colors = 2;  // colour 4 still stored, but past the count
    scene_color(s, 3, rgb);
    EXPECT_EQ(rgb[0], 0);
    s.num_colors = 200;
    EXPECT_EQ(scene_num_colors(s), kSceneColorsMax);
}

static void test_scene_bank_load_all_layouts() {
    static SceneBank bank;

    // v1: 8 × 25-byte records.
    SceneV1 v1[kLegacyNumScenes]{};
    std::strcpy(v1[3].name, "v1");
    EXPECT_TRUE(load_scene_bank(reinterpret_cast<const uint8_t*>(v1), sizeof(v1), bank));
    EXPECT_EQ(bank.count, kLegacyNumScenes);
    EXPECT_TRUE(std::strcmp(bank.scenes[3].name, "v1") == 0);

    // v2: 8 fixed 34-byte records, no count byte.
    Scene v2[kLegacyNumScenes]{};
    std::strcpy(v2[7].name, "v2");
    v2[7].num_colors = 3;
    v2[7].effect     = 200;  // out of range → sanitized
    EXPECT_TRUE(load_scene_bank(reinterpret_cast<const uint8_t*>(v2), sizeof(v2), bank));
    EXPECT_EQ(bank.count, kLegacyNumScenes);
    EXPECT_TRUE(std::strcmp(bank.scenes[7].name, "v2") == 0);
    EXPECT_EQ(bank.scenes[7].num_colors, 3);
    EXPECT_EQ(bank.scenes[7].effect, kSceneFxSolid);

    // v3: count byte + that many records, any count 0..kMaxScenes.
    static SceneBank src;
    std::memset(&src, 0, sizeof(src));
    src.count = 12;
    std::strcpy(src.scenes[11].name, "twelfth");
    EXPECT_TRUE(
        load_scene_bank(reinterpret_cast<const uint8_t*>(&src), scene_bank_bytes(12), bank));
    EXPECT_EQ(bank.count, 12);
    EXPECT_TRUE(std::strcmp(bank.scenes[11].name, "twelfth") == 0);
    src.count = 0;
    EXPECT_TRUE(load_scene_bank(reinterpret_cast<const uint8_t*>(&src), scene_bank_bytes(0), bank));
    EXPECT_EQ(bank.count, 0);

    // Count byte disagreeing with the size, or past the capacity → rejected.
    src.count = 5;
    EXPECT_TRUE(
        !load_scene_bank(reinterpret_cast<const uint8_t*>(&src), scene_bank_bytes(4), bank));
    src.count = kMaxScenes + 1;
    EXPECT_TRUE(!load_scene_bank(reinterpret_cast<const uint8_t*>(&src), sizeof(src), bank));
    // The three layouts can never be mistaken for one another.
    for (size_t k = 0; k <= kMaxScenes; ++k) {
        EXPECT_TRUE(scene_bank_bytes(k) != kLegacyNumScenes * kSceneV1Size);
        EXPECT_TRUE(scene_bank_bytes(k) != kLegacyNumScenes * sizeof(Scene));
    }
}

static void test_scene_index_remap() {
    // Delete scene 2 of [0 1 2 3 4].
    EXPECT_EQ(remap_scene_index(1, SceneEdit::Delete, 2), 1);
    EXPECT_EQ(remap_scene_index(2, SceneEdit::Delete, 2), -1);
    EXPECT_EQ(remap_scene_index(3, SceneEdit::Delete, 2), 2);
    EXPECT_EQ(remap_scene_index(-1, SceneEdit::Delete, 2), -1);
    // Move 1 → 3: [0 2 3 1 4].
    EXPECT_EQ(remap_scene_index(1, SceneEdit::Move, 1, 3), 3);
    EXPECT_EQ(remap_scene_index(2, SceneEdit::Move, 1, 3), 1);
    EXPECT_EQ(remap_scene_index(3, SceneEdit::Move, 1, 3), 2);
    EXPECT_EQ(remap_scene_index(0, SceneEdit::Move, 1, 3), 0);
    EXPECT_EQ(remap_scene_index(4, SceneEdit::Move, 1, 3), 4);
    // Move 3 → 1: [0 3 1 2 4].
    EXPECT_EQ(remap_scene_index(3, SceneEdit::Move, 3, 1), 1);
    EXPECT_EQ(remap_scene_index(1, SceneEdit::Move, 3, 1), 2);
    EXPECT_EQ(remap_scene_index(2, SceneEdit::Move, 3, 1), 3);
    EXPECT_EQ(remap_scene_index(4, SceneEdit::Move, 3, 1), 4);
    EXPECT_EQ(remap_scene_index(2, SceneEdit::Move, 2, 2), 2);
}

int main() {
    test_migrate_from_pre_web_fields_preserved();
    test_migrate_from_pre_web_new_field_is_false();
    test_migrate_backlight_defaults_to_full_brightness();
    test_migrate_exact_size_ok();
    test_migrate_rejects_downgrade();
    test_migrate_zero_size_blob_all_zero();
    test_channel_migration_sanitizes_to_identity();
    test_sanitize_clamps_clock_hz();
    test_scene_v1_migration();
    test_scene_colour_accessors();
    test_scene_bank_load_all_layouts();
    test_scene_index_remap();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
