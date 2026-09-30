// config_store.cpp — the real NVS load/save/migration paths against the
// in-memory NVS shim. Every case starts from wiped flash and a fresh init()
// ("a reboot"): init() reloads every cached struct from NVS.

#include <cstring>

#include "config_store.h"
#include "harness.h"
#include "mbedtls/sha256.h"
#include "shim_control.h"

using namespace pixfrog::config;

namespace {

constexpr const char* kNs = "pixfrog";

void fresh_boot() {
    shim::nvs_wipe();
    init();
}

template <typename T> T load_raw(const char* key) {
    T t{};
    const auto raw = shim::nvs_raw(kNs, key);
    std::memcpy(&t, raw.data(), raw.size() < sizeof(T) ? raw.size() : sizeof(T));
    return t;
}

}  // namespace

TEST(sha256_shim_matches_fips_vectors) {
    auto hex = [](const char* in) {
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, reinterpret_cast<const unsigned char*>(in), std::strlen(in));
        unsigned char d[32];
        mbedtls_sha256_finish(&c, d);
        static char s[65];
        for (int i = 0; i < 32; ++i)
            std::snprintf(s + 2 * i, 3, "%02x", d[i]);
        return s;
    };
    EXPECT_STREQ(hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_STREQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(fresh_flash_gets_defaults_persisted) {
    fresh_boot();
    EXPECT_TRUE(is_persistence_ok());
    EXPECT_TRUE(shim::nvs_has(kNs, "global"));
    for (char k : { '0', '7' })
        EXPECT_TRUE(shim::nvs_has(kNs, std::string("ch") + k));
    EXPECT_TRUE(shim::nvs_has(kNs, "scenes"));
    EXPECT_EQ(get_global().refresh_rate_hz, kDefaultRefreshHz);
    EXPECT_EQ(num_scenes(), kLegacyNumScenes);
    EXPECT_EQ(shim::nvs_raw(kNs, "scenes").size(), scene_bank_bytes(kLegacyNumScenes));
}

TEST(channel_survives_reboot_and_gaps_are_normalized) {
    fresh_boot();
    auto c           = get_channel(3);
    c.protocol       = pixfrog::led::Protocol::WS2815;
    c.pixel_count    = 300;
    c.universe_start = 42;
    c.gaps[0]        = { 200, 2 };
    c.gaps[1]        = { 0, 1 };
    c.gaps[2]        = { 201, 3 };  // overlaps the first → merged
    EXPECT_TRUE(set_channel(3, c));
    init();  // reboot
    const auto& r = get_channel(3);
    EXPECT_EQ(r.pixel_count, 300);
    EXPECT_EQ(r.universe_start, 42);
    EXPECT_EQ(r.gaps[0].pos, 0);
    EXPECT_EQ(r.gaps[1].pos, 200);
    EXPECT_EQ(r.gaps[1].len, 4);
    EXPECT_EQ(r.gaps[2].len, 0);
}

TEST(smaller_global_blob_migrates_and_is_resaved) {
    fresh_boot();
    GlobalConfig g = get_global();
    std::strcpy(g.short_name, "old-box");
    g.refresh_rate_hz = 30;
    // An older firmware stored a shorter struct: keep a prefix only.
    const size_t old_size = offsetof(GlobalConfig, web_enabled);
    shim::nvs_put_raw(kNs, "global", &g, old_size);
    init();
    EXPECT_STREQ(get_global().short_name, "old-box");
    EXPECT_EQ(get_global().refresh_rate_hz, 30);
    EXPECT_FALSE(get_global().web_enabled);  // new field: zero default
    EXPECT_EQ(shim::nvs_raw(kNs, "global").size(), sizeof(GlobalConfig));
}

// Fields appended after the blob was written read as zero; the header helpers
// must turn those zeros into the documented defaults, through the real load.
TEST(zero_filled_tails_read_as_defaults) {
    fresh_boot();
    const GlobalConfig g = get_global();
    shim::nvs_put_raw(kNs, "global", &g, offsetof(GlobalConfig, tft_brightness));
    ChannelConfig c = get_channel(2);
    c.protocol      = pixfrog::led::Protocol::WS2815;
    c.brightness    = 200;
    shim::nvs_put_raw(kNs, "ch2", &c, offsetof(ChannelConfig, gamma_x10));
    init();
    EXPECT_EQ(tft_brightness_pct(get_global()), 100);
    EXPECT_EQ(get_channel(2).gamma_x10, 10);    // linear
    EXPECT_EQ(get_channel(2).wb_r, 255);        // unity white balance
    EXPECT_EQ(get_channel(2).brightness, 200);  // old field kept
    EXPECT_EQ(get_channel(2).gaps[0].len, 0);   // no gap
}

// Protocol 9 was DMX512 output (moved to the DMX node firmware): a channel
// stored with it must load disabled, not emit its slot count as LED pixels.
TEST(retired_dmx512_protocol_loads_as_off) {
    fresh_boot();
    ChannelConfig c = get_channel(4);
    c.protocol      = static_cast<pixfrog::led::Protocol>(9);
    c.pixel_count   = 512;
    shim::nvs_put_raw(kNs, "ch4", &c, sizeof(c));
    init();
    EXPECT_TRUE(get_channel(4).protocol == pixfrog::led::Protocol::Off);
    EXPECT_EQ(get_channel(4).pixel_count, 512);  // the rest is kept
}

TEST(larger_global_blob_is_rejected_for_defaults) {
    fresh_boot();
    std::vector<uint8_t> big(sizeof(GlobalConfig) + 16, 0xAB);
    shim::nvs_put_raw(kNs, "global", big.data(), big.size());
    init();
    EXPECT_EQ(get_global().refresh_rate_hz, kDefaultRefreshHz);
    EXPECT_EQ(shim::nvs_raw(kNs, "global").size(), sizeof(GlobalConfig));
}

TEST(out_of_range_refresh_falls_back_to_default) {
    fresh_boot();
    auto g            = load_raw<GlobalConfig>("global");
    g.refresh_rate_hz = 7;
    shim::nvs_put_raw(kNs, "global", &g, sizeof(g));
    init();
    EXPECT_EQ(get_global().refresh_rate_hz, kDefaultRefreshHz);
    EXPECT_EQ(load_raw<GlobalConfig>("global").refresh_rate_hz, kDefaultRefreshHz);
}

TEST(scene_blob_v1_migrates_through_the_real_load_path) {
    fresh_boot();
    uint8_t v1[kLegacyNumScenes * kSceneV1Size] = {};
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        uint8_t* rec = v1 + i * kSceneV1Size;
        std::snprintf(reinterpret_cast<char*>(rec), kSceneNameMax, "v1-%u", unsigned(i));
        rec[16] = 0xFF;                                    // mask
        rec[17] = i == 1 ? kSceneFxChase : kSceneFxSolid;  // effect
        rec[21] = 90;                                      // speed
    }
    shim::nvs_put_raw(kNs, "scenes", v1, sizeof(v1));
    init();
    EXPECT_EQ(num_scenes(), kLegacyNumScenes);
    EXPECT_STREQ(get_scene(5).name, "v1-5");
    EXPECT_EQ(get_scene(0).speed, 0);   // solid: strobe must not start
    EXPECT_EQ(get_scene(1).speed, 90);  // chase keeps its speed
    EXPECT_EQ(shim::nvs_raw(kNs, "scenes").size(), scene_bank_bytes(kLegacyNumScenes));
}

TEST(scene_blob_v2_and_garbage) {
    fresh_boot();
    Scene v2[kLegacyNumScenes] = {};
    std::strcpy(v2[6].name, "v2-six");
    v2[6].num_colors = 2;
    shim::nvs_put_raw(kNs, "scenes", v2, sizeof(v2));
    init();
    EXPECT_EQ(num_scenes(), kLegacyNumScenes);
    EXPECT_STREQ(get_scene(6).name, "v2-six");
    EXPECT_EQ(scene_num_colors(get_scene(6)), 2);

    const uint8_t junk[37] = { 5 };
    shim::nvs_put_raw(kNs, "scenes", junk, sizeof(junk));
    init();
    EXPECT_EQ(num_scenes(), kLegacyNumScenes);  // defaults
    EXPECT_STREQ(get_scene(0).name, "Warm white");
}

TEST(scene_list_edits_persist_and_remap_references) {
    fresh_boot();
    auto g           = get_global();
    g.boot_scene     = 3;  // scene index 2
    g.failsafe_scene = 5;
    g.failsafe_mode  = kFailsafeScene;
    set_global(g);

    Scene s{};
    std::strcpy(s.name, "extra");
    EXPECT_EQ(add_scene(s), 8);
    EXPECT_TRUE(move_scene(8, 0));  // everything shifts down by one
    EXPECT_EQ(get_global().boot_scene, 4);
    EXPECT_EQ(get_global().failsafe_scene, 6);
    EXPECT_TRUE(delete_scene(6));  // the failsafe scene goes
    EXPECT_EQ(get_global().failsafe_mode, kFailsafeBlackout);
    EXPECT_TRUE(delete_scene(3));  // the boot scene goes
    EXPECT_EQ(get_global().boot_scene, 0);

    init();  // reboot: all of it came from NVS
    EXPECT_EQ(num_scenes(), 7);
    EXPECT_STREQ(get_scene(0).name, "extra");
    EXPECT_EQ(get_global().boot_scene, 0);
    EXPECT_EQ(get_global().failsafe_mode, kFailsafeBlackout);
}

TEST(scene_list_capacity_and_bounds) {
    fresh_boot();
    Scene s{};
    while (num_scenes() < kMaxScenes)
        EXPECT_TRUE(add_scene(s) >= 0);
    EXPECT_EQ(add_scene(s), -1);
    EXPECT_FALSE(delete_scene(kMaxScenes));
    EXPECT_FALSE(move_scene(0, kMaxScenes));
    EXPECT_FALSE(set_scene(kMaxScenes, s));
    EXPECT_EQ(get_scene(99).channel_mask, 0);  // blank, not an alias of scene 0

    static Scene many[kMaxScenes + 5];
    EXPECT_TRUE(replace_scenes(many, kMaxScenes + 5));
    EXPECT_EQ(num_scenes(), kMaxScenes);
    s.effect = 200;  // out of range → sanitized on write
    EXPECT_TRUE(set_scene(0, s));
    EXPECT_EQ(get_scene(0).effect, kSceneFxSolid);
}

TEST(web_password_hash_and_check) {
    fresh_boot();
    EXPECT_FALSE(web_password_set());
    EXPECT_TRUE(set_web_password("s3cret"));
    EXPECT_TRUE(web_password_set());
    EXPECT_TRUE(check_web_password("s3cret"));
    EXPECT_FALSE(check_web_password("s3cre"));
    EXPECT_FALSE(check_web_password(""));
    init();  // reboot: hash + salt persisted
    EXPECT_TRUE(check_web_password("s3cret"));
    EXPECT_TRUE(set_web_password(""));  // clears
    EXPECT_FALSE(web_password_set());
}

TEST(nvs_init_failure_recovers_by_erasing) {
    shim::nvs_wipe();
    shim::nvs_fail_init(1);  // first init fails, erase + retry succeeds
    init();
    EXPECT_TRUE(is_persistence_ok());
    EXPECT_TRUE(shim::nvs_has(kNs, "global"));
}

TEST(nvs_unrecoverable_runs_on_ram_defaults) {
    shim::nvs_wipe();
    shim::nvs_fail_init(5);
    shim::nvs_fail_erase(5);
    init();
    EXPECT_FALSE(is_persistence_ok());
    EXPECT_EQ(get_global().refresh_rate_hz, kDefaultRefreshHz);
    auto c        = get_channel(0);
    c.pixel_count = 77;
    EXPECT_FALSE(set_channel(0, c));            // not persisted…
    EXPECT_EQ(get_channel(0).pixel_count, 77);  // …but the cache follows
}

TEST(nvs_open_failure_recovers) {
    shim::nvs_wipe();
    shim::nvs_fail_open(1);
    init();
    EXPECT_TRUE(is_persistence_ok());
}

TEST(rollback_record_roundtrip_survives_factory_reset) {
    fresh_boot();
    RollbackRecord r{};
    EXPECT_FALSE(get_rollback(r));
    std::strcpy(r.rejected_version, "v9.9.9");
    std::strcpy(r.rejected_slot, "ota_1");
    r.reset_reason = 4;
    EXPECT_TRUE(set_rollback(r));
    reset_to_defaults();
    RollbackRecord back{};
    EXPECT_TRUE(get_rollback(back));
    EXPECT_STREQ(back.rejected_version, "v9.9.9");
    EXPECT_EQ(back.reset_reason, 4);
}

TEST(factory_reset_restores_defaults_in_nvs) {
    fresh_boot();
    auto c        = get_channel(1);
    c.pixel_count = 999;
    set_channel(1, c);
    Scene s{};
    add_scene(s);
    reset_to_defaults();
    init();
    EXPECT_TRUE(get_channel(1).pixel_count != 999);
    EXPECT_EQ(num_scenes(), kLegacyNumScenes);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv);
}

// ── DMX control universe + scene fade ───────────────────────────────────────

TEST(control_defaults_off_and_round_trip_through_nvs) {
    fresh_boot();
    const ControlConfig& d = get_control();
    EXPECT_EQ(d.enabled, 0);  // an upgrade changes nothing until switched on
    EXPECT_EQ(d.universe, kDefaultControlUniverse);
    EXPECT_EQ(d.count, 5);
    EXPECT_TRUE(shim::nvs_has(kNs, "control"));

    ControlConfig c = d;
    c.enabled       = 1;
    c.universe      = 321;
    c.address       = 40;
    control_apply_preset(c, ControlPreset::Full);
    c.slots[4] = control_slot(CtlFn::Scene, 0x0F);
    EXPECT_TRUE(set_control(c));
    init();  // reboot
    EXPECT_EQ(get_control().enabled, 1);
    EXPECT_EQ(get_control().universe, 321);
    EXPECT_EQ(get_control().address, 40);
    EXPECT_EQ(get_control().count, 15);
    EXPECT_EQ(get_control().slots[4].mask, 0x0F);
}

TEST(corrupt_control_blob_is_sanitized_on_load) {
    fresh_boot();
    ControlConfig c = default_control();
    c.count         = 200;
    c.address       = 999;
    c.slots[0].fn   = 77;
    shim::nvs_put_raw(kNs, "control", &c, sizeof(c));
    init();
    EXPECT_EQ(get_control().count, kMaxControlSlots);
    EXPECT_EQ(get_control().address, 1);
    EXPECT_TRUE(get_control().slots[0].fn == static_cast<uint8_t>(CtlFn::None));
}

TEST(scene_fade_is_a_zero_filled_global_tail) {
    fresh_boot();
    GlobalConfig g = get_global();
    shim::nvs_put_raw(kNs, "global", &g, offsetof(GlobalConfig, scene_fade_ms));
    init();
    EXPECT_EQ(get_global().scene_fade_ms, 0);  // instant, as before fades
    g               = get_global();
    g.scene_fade_ms = 60000;  // out of range from a bad writer
    shim::nvs_put_raw(kNs, "global", &g, sizeof(g));
    init();
    EXPECT_EQ(get_global().scene_fade_ms, kMaxSceneFadeMs);
}

TEST(factory_reset_restores_the_control_default) {
    fresh_boot();
    ControlConfig c = get_control();
    c.enabled       = 1;
    set_control(c);
    reset_to_defaults();
    EXPECT_EQ(get_control().enabled, 0);
    init();
    EXPECT_EQ(get_control().enabled, 0);
}
