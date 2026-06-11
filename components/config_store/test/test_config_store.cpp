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

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    test_migrate_from_pre_web_fields_preserved();
    test_migrate_from_pre_web_new_field_is_false();
    test_migrate_exact_size_ok();
    test_migrate_rejects_downgrade();
    test_migrate_zero_size_blob_all_zero();
    test_channel_migration_sanitizes_to_identity();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
