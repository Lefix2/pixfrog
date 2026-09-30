// dmx_manager.cpp — the real universe banks, per-channel decode and override
// chain (identify → preview → scene → FSEQ → failsafe → network), driven by
// the fake clock. dmx::init() allocates once per process; each case reshapes
// the config it needs and resets the runtime state it touches.

#include <cstring>

#include "config_store.h"
#include "dmx_manager.h"
#include "harness.h"
#include "shim_control.h"

using namespace pixfrog;

namespace {

constexpr uint32_t kSrcA = 0x0A000001, kSrcB = 0x0A000002, kSrcC = 0x0A000003;

void apply_channels() {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        dmx::mark_channel_dirty(ch);
    dmx::handle_pending_remaps();
}

// Channel 0: WS2815, `px` pixels from universe 1; every other channel Off.
void one_channel(uint16_t px = 4, uint16_t dmx_start = 1) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch == 0 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = px;
        c.universe_start = static_cast<uint16_t>(1 + ch * 8);
        c.dmx_start      = dmx_start;
        c.color_order    = led::ColorOrder::RGB;
        std::memset(c.gaps, 0, sizeof(c.gaps));
        config::set_channel(ch, c);
    }
    apply_channels();
}

void set_failsafe(uint8_t mode, uint16_t timeout_s) {
    auto g               = config::get_global();
    g.failsafe_mode      = mode;
    g.failsafe_timeout_s = timeout_s;
    g.failsafe_r         = 9;
    g.failsafe_g         = 8;
    g.failsafe_b         = 7;
    g.merge_mode         = config::kMergeHtp;
    g.refresh_rate_hz    = 60;
    config::set_global(g);
}

// Push `data` for universe `u` from `src`, publish it, decode channel 0.
const uint8_t* frame(uint16_t u, const uint8_t* data, size_t len, uint32_t src = kSrcA) {
    dmx::write_universe_from_source(u, data, len, src, dmx::kArtnetMergeTimeoutUs);
    dmx::note_channel_activity(0);
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(0);
    return dmx::pixel_back_buffer(0);
}

const uint8_t* decode0() {
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(0);
    return dmx::pixel_back_buffer(0);
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        once = true;
    }
    dmx::scene_stop();
    dmx::identify_stop();
    dmx::clear_pixel_preview();
    dmx::fseq_set_active(false);
    dmx::merge_cancel_all();
    set_failsafe(config::kFailsafeHold, 0);
    one_channel();
    shim::advance_ms(1000);
}

}  // namespace

TEST(network_frame_reaches_the_pixel_buffer) {
    const uint8_t d[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    const uint8_t* px   = frame(1, d, sizeof(d));
    EXPECT_TRUE(std::memcmp(px, d, 12) == 0);
    EXPECT_EQ(dmx::channel_for_universe(1), 0);
    EXPECT_EQ(dmx::channel_for_universe(2), -1);
}

// Regression for the bank-swap flicker: with nothing new, the front bank must
// stay put rather than alternate with the previous source frame.
TEST(no_new_data_holds_the_last_frame) {
    const uint8_t a[3] = { 10, 20, 30 }, b[3] = { 40, 50, 60 };
    frame(1, a, 3);
    frame(1, b, 3);
    for (int i = 0; i < 3; ++i) {
        const uint8_t* px = decode0();
        EXPECT_EQ(px[0], 40);
    }
}

TEST(short_packet_keeps_the_rest_of_the_universe) {
    const uint8_t full[12] = { 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4 };
    frame(1, full, 12);
    const uint8_t head[3] = { 9, 9, 9 };
    const uint8_t* px     = frame(1, head, 3);
    EXPECT_EQ(px[0], 9);
    EXPECT_EQ(px[3], 2);  // seeded from the previous frame, not zero
    EXPECT_EQ(px[11], 4);
}

TEST(dmx_start_offset_and_universe_spanning) {
    one_channel(200, 4);  // 600 bytes from slot 4 of U1 → spills into U2
    uint8_t u1[512], u2[512];
    for (int i = 0; i < 512; ++i) {
        u1[i] = static_cast<uint8_t>(i);
        u2[i] = static_cast<uint8_t>(0x80 + i);
    }
    dmx::write_universe_from_source(1, u1, 512, kSrcA, dmx::kArtnetMergeTimeoutUs);
    dmx::write_universe_from_source(2, u2, 512, kSrcA, dmx::kArtnetMergeTimeoutUs);
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 3);          // slot 4 = index 3
    EXPECT_EQ(px[508], u1[511]);  // last byte of U1
    EXPECT_EQ(px[509], u2[0]);    // continues in U2
}

TEST(unmapped_universe_is_dropped) {
    const uint8_t d[3] = { 1, 2, 3 };
    EXPECT_FALSE(dmx::write_universe_from_source(500, d, 3, kSrcA, dmx::kArtnetMergeTimeoutUs));
}

TEST(failsafe_blackout_after_timeout_on_the_fake_clock) {
    set_failsafe(config::kFailsafeBlackout, 2);
    const uint8_t d[3] = { 50, 60, 70 };
    frame(1, d, 3);
    shim::advance_ms(1900);
    EXPECT_EQ(decode0()[0], 50);  // still inside the timeout
    EXPECT_FALSE(dmx::is_channel_failsafe(0));
    shim::advance_ms(200);
    EXPECT_EQ(decode0()[0], 0);
    EXPECT_TRUE(dmx::is_channel_failsafe(0));
    EXPECT_EQ(frame(1, d, 3)[0], 50);  // data comes back → recovery
}

TEST(failsafe_never_fires_on_a_never_driven_channel) {
    set_failsafe(config::kFailsafeColor, 1);
    EXPECT_FALSE(dmx::is_channel_failsafe(5));
}

TEST(failsafe_colour_fill) {
    set_failsafe(config::kFailsafeColor, 1);
    const uint8_t d[3] = { 1, 1, 1 };
    frame(1, d, 3);
    shim::advance_ms(1500);
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 9);
    EXPECT_EQ(px[1], 8);
    EXPECT_EQ(px[2], 7);
}

TEST(failsafe_scene_honours_the_scene_mask) {
    config::Scene s{};
    s.effect       = config::kSceneFxSolid;
    s.r            = 33;
    s.num_colors   = 1;
    s.channel_mask = 0x02;  // not channel 0
    config::set_scene(0, s);
    auto g           = config::get_global();
    g.failsafe_scene = 0;
    config::set_global(g);
    set_failsafe(config::kFailsafeScene, 1);
    const uint8_t d[3] = { 1, 1, 1 };
    frame(1, d, 3);
    shim::advance_ms(1500);
    EXPECT_EQ(decode0()[0], 0);  // outside the mask: blackout
    s.channel_mask = 0x01;
    config::set_scene(0, s);
    EXPECT_EQ(decode0()[0], 33);
}

TEST(scene_overrides_the_network_until_stopped) {
    config::Scene s{};
    s.effect       = config::kSceneFxSolid;
    s.g            = 77;
    s.num_colors   = 1;
    s.channel_mask = 0xFF;
    config::set_scene(1, s);
    const uint8_t d[3] = { 5, 5, 5 };
    frame(1, d, 3);
    dmx::scene_start(1);
    EXPECT_EQ(dmx::active_scene(), 1);
    EXPECT_EQ(frame(1, d, 3)[1], 77);
    dmx::scene_stop();
    EXPECT_EQ(frame(1, d, 3)[1], 5);
    dmx::scene_start(200);  // out of range: ignored
    EXPECT_EQ(dmx::active_scene(), -1);
}

TEST(playing_scene_follows_list_edits) {
    dmx::scene_start(4);
    dmx::scene_list_edited(config::SceneEdit::Move, 4, 1);
    EXPECT_EQ(dmx::active_scene(), 1);
    dmx::scene_list_edited(config::SceneEdit::Delete, 0);
    EXPECT_EQ(dmx::active_scene(), 0);
    dmx::scene_list_edited(config::SceneEdit::Delete, 0);
    EXPECT_EQ(dmx::active_scene(), -1);
}

TEST(identify_blinks_at_2hz_and_expires) {
    dmx::identify_start(0, 2);
    shim::set_time_us((shim::now_us() / 500000 + 1) * 500000);  // align on a 500 ms edge
    const uint8_t first = decode0()[0];
    shim::advance_ms(250);
    const uint8_t second = decode0()[0];
    EXPECT_TRUE(first != second);
    EXPECT_TRUE(first == 255 || second == 255);
    shim::advance_ms(2500);
    EXPECT_EQ(dmx::identify_channel(), -1);
}

TEST(htp_merge_of_two_sources_and_third_rejected) {
    const uint8_t a[3] = { 100, 0, 50 }, b[3] = { 20, 90, 60 }, c[3] = { 255, 255, 255 };
    dmx::write_universe_from_source(1, a, 3, kSrcA, dmx::kArtnetMergeTimeoutUs);
    dmx::write_universe_from_source(1, b, 3, kSrcB, dmx::kArtnetMergeTimeoutUs);
    EXPECT_FALSE(dmx::write_universe_from_source(1, c, 3, kSrcC, dmx::kArtnetMergeTimeoutUs));
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 100);
    EXPECT_EQ(px[1], 90);
    EXPECT_EQ(px[2], 60);
    EXPECT_TRUE(dmx::is_channel_merging(0));
    shim::advance_us(dmx::kArtnetMergeTimeoutUs + 1000);  // B goes quiet
    dmx::write_universe_from_source(1, a, 3, kSrcA, dmx::kArtnetMergeTimeoutUs);
    EXPECT_EQ(decode0()[1], 0);
    EXPECT_FALSE(dmx::is_channel_merging(0));
}

TEST(pixel_budget_is_not_destructive) {
    one_channel(1024);
    auto g            = config::get_global();
    g.refresh_rate_hz = 60;
    config::set_global(g);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    EXPECT_EQ(dmx::effective_channel(0).pixel_count, 512);
    EXPECT_EQ(config::get_channel(0).pixel_count, 1024);
    EXPECT_FALSE(dmx::is_channel_capacity_ok(0));
    g.refresh_rate_hz = 30;
    config::set_global(g);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    EXPECT_EQ(dmx::effective_channel(0).pixel_count, 1024);
    EXPECT_TRUE(dmx::is_channel_capacity_ok(0));
}

TEST(frame_emit_time_is_the_longest_channel) {
    one_channel(4);
    auto c        = config::get_channel(1);
    c.protocol    = led::Protocol::WS2815;
    c.pixel_count = 300;  // 300 × 480 + 4480 reset samples at 16 MHz = 9 280 µs
    config::set_channel(1, c);
    apply_channels();
    EXPECT_EQ(dmx::frame_emit_us(), 9280u);
}

TEST(pixel_preview_ruler_and_erase_tail) {
    dmx::set_pixel_preview(0, 20);
    decode0();
    EXPECT_EQ(dmx::preview_emit_count(), 20);
    dmx::set_pixel_preview(0, 8);  // shrink: one frame blanks the dropped LEDs
    decode0();
    EXPECT_EQ(dmx::preview_emit_count(), 20);
    decode0();
    EXPECT_EQ(dmx::preview_emit_count(), 8);
    const led::PixelGap gaps[1] = { { 0, 2 } };
    dmx::set_preview_gaps(gaps, 1);
    decode0();
    EXPECT_EQ(dmx::preview_emit_count(), 10);  // 8 live + 2 dead, physical
    dmx::clear_pixel_preview();
    EXPECT_EQ(dmx::pixel_preview_channel(), -1);
}

TEST(inject_writes_both_banks) {
    const uint8_t d[3] = { 3, 2, 1 };
    EXPECT_TRUE(dmx::inject_universe(1, 0, d, 3));
    EXPECT_EQ(decode0()[0], 3);
    EXPECT_FALSE(dmx::inject_universe(1, 511, d, 3));  // past the universe
}

TEST(auto_patch_lays_channels_out_contiguously) {
    for (size_t ch = 0; ch < 3; ++ch) {
        auto c        = config::get_channel(ch);
        c.protocol    = led::Protocol::WS2815;
        c.pixel_count = 200;  // 600 B → 2 universes each
        config::set_channel(ch, c);
    }
    uint16_t next = 0;
    EXPECT_TRUE(dmx::auto_patch_universes(10, &next));
    EXPECT_EQ(config::get_channel(0).universe_start, 10);
    EXPECT_EQ(config::get_channel(1).universe_start, 12);
    EXPECT_EQ(config::get_channel(2).universe_start, 14);
    EXPECT_EQ(next, 16);
}

TEST(artsync_wakes_the_render_wait) {
    const int64_t t0 = shim::now_us();
    EXPECT_FALSE(dmx::wait_for_sync_or_period(10));
    EXPECT_EQ(shim::now_us() - t0, 10000);  // timed out on the fake clock
    dmx::note_sync();
    EXPECT_TRUE(dmx::wait_for_sync_or_period(10));
}

TEST(telemetry_counters) {
    const auto before = dmx::get_stats();
    dmx::note_packet_rx();
    dmx::note_packet_bad();
    dmx::note_frame_emitted();
    dmx::set_current_fps(42);
    const auto after = dmx::get_stats();
    EXPECT_EQ(after.artnet_packets_rx, before.artnet_packets_rx + 1);
    EXPECT_EQ(after.artnet_bad_packets, before.artnet_bad_packets + 1);
    EXPECT_EQ(after.frames_emitted, before.frames_emitted + 1);
    EXPECT_EQ(after.current_fps, 42);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
