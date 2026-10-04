// dmx_manager.cpp — the real universe banks, per-channel decode and override
// chain (identify → preview → scene → FSEQ → failsafe → network), driven by
// the fake clock. dmx::init() allocates once per process; each case reshapes
// the config it needs and resets the runtime state it touches.

#include <cstdio>
#include <cstring>
#include <string>

#include "config_store.h"
#include "dmx_logic.h"
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

// Control universe off, local show values neutral, no crossfade.
void reset_show() {
    config::set_control(config::default_control());
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    dmx::update_show_control();  // releases anything a live desk left behind
    dmx::master_set(dmx::kAllOutputs, dmx::kMasterFull);
    dmx::blackout_set(dmx::kAllOutputs, false);
    dmx::strobe_set(dmx::kAllOutputs, 0);
    auto g          = config::get_global();
    g.scene_fade_ms = 0;
    config::set_global(g);
    dmx::take_fseq_request();
}

constexpr uint16_t kCtrlUni = 100;

void enable_control(config::ControlPreset p = config::ControlPreset::Simple,
                    uint16_t universe = kCtrlUni, uint16_t address = 1) {
    auto c     = config::default_control();
    c.enabled  = 1;
    c.universe = universe;
    c.address  = address;
    config::control_apply_preset(c, p);
    config::set_control(c);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
}

// One desk frame on the control universe, published and evaluated the way
// render_task does it.
void ctrl_frame(const uint8_t* d, size_t len, uint16_t universe = kCtrlUni) {
    dmx::write_universe_from_source(universe, d, len, kSrcA, dmx::kArtnetMergeTimeoutUs);
    dmx::swap_universes();
    dmx::update_show_control();
}

// Scene `idx` becomes a static solid colour on every output (tests own their
// scenes: earlier cases edit the defaults).
void solid_scene(size_t idx, uint8_t r, uint8_t g, uint8_t b) {
    config::Scene sc{};
    std::snprintf(sc.name, sizeof(sc.name), "Solid %u", static_cast<unsigned>(idx));
    sc.channel_mask = 0xFF;
    sc.effect       = config::kSceneFxSolid;
    sc.num_colors   = 1;
    config::set_scene_color(sc, 0, r, g, b);
    config::set_scene(idx, sc);
}

// Fake clock to the start of the next `period_ms` window.
void align_ms(uint32_t period_ms) {
    const auto now = static_cast<uint32_t>(shim::now_us() / 1000);
    shim::advance_ms(period_ms - now % period_ms);
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        once = true;
    }
    reset_show();
    dmx::scene_stop();
    dmx::identify_stop();
    dmx::sync_reset();
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

// Three blinks at 2 Hz (lit first), then the output is back to its show.
TEST(identify_blinks_three_times_then_ends) {
    one_channel(4);
    dmx::identify_start(0);
    std::string seen;
    for (int i = 0; i < 7; ++i, shim::advance_ms(250))
        seen += decode0()[0] == 255 ? '#' : '.';
    EXPECT_STREQ(seen.c_str(), "#.#.#..");
    EXPECT_EQ(dmx::identify_channel(), -1);  // 1.5 s: done
}

// The dashboard's identify: every configured output in turn, in channel order.
TEST(identify_runs_the_outputs_one_after_the_other) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c     = config::get_channel(ch);
        c.protocol = (ch == 1 || ch == 4) ? led::Protocol::WS2815 : led::Protocol::Off;
        config::set_channel(ch, c);
    }
    EXPECT_EQ(dmx::identify_configured_outputs(), 0x12);
    dmx::identify_outputs(dmx::identify_configured_outputs());
    EXPECT_EQ(dmx::identify_channel(), 1);
    EXPECT_TRUE(dmx::identify_lit());
    shim::advance_ms(250);
    EXPECT_FALSE(dmx::identify_lit());
    shim::advance_ms(1250);  // output 2's three blinks are over
    EXPECT_EQ(dmx::identify_channel(), 4);
    EXPECT_TRUE(dmx::identify_lit());
    shim::advance_ms(1500);
    EXPECT_EQ(dmx::identify_channel(), -1);
    EXPECT_FALSE(dmx::identify_lit());
    dmx::identify_outputs(0);  // nothing configured: nothing to do
    EXPECT_EQ(dmx::identify_channel(), -1);
    dmx::identify_start(9);  // out of range: ignored
    EXPECT_EQ(dmx::identify_channel(), -1);
    one_channel();
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
    // An enabled control universe follows the outputs, from address 1.
    auto ctl    = config::get_control();
    ctl.enabled = 1;
    ctl.address = 40;
    config::set_control(ctl);
    EXPECT_TRUE(dmx::auto_patch_universes(10, &next));
    EXPECT_EQ(config::get_control().universe, 16);
    EXPECT_EQ(config::get_control().address, 1);
    EXPECT_EQ(next, 17);
    // Nowhere left past the last universe: the control universe stays put.
    for (size_t ch = 1; ch < 3; ++ch) {
        auto c     = config::get_channel(ch);
        c.protocol = led::Protocol::Off;
        config::set_channel(ch, c);
    }
    EXPECT_TRUE(dmx::auto_patch_universes(dmx::kMaxUniverseNumber - 1, &next));
    EXPECT_EQ(config::get_control().universe, 16);
    ctl.enabled = 0;
    config::set_control(ctl);
}

// Compact: channels follow each other inside a universe and share it; the
// control universe takes the room left. Data on the shared universe makes
// both outputs active and each decodes its own slots.
TEST(compact_auto_patch_shares_universes) {
    config::ChannelConfig saved[3];
    for (size_t ch = 0; ch < 3; ++ch) {
        saved[ch]     = config::get_channel(ch);
        auto c        = saved[ch];
        c.protocol    = led::Protocol::WS2815;
        c.pixel_count = 50;  // 150 B
        c.grouping    = 1;
        config::set_channel(ch, c);
    }
    for (size_t ch = 3; ch < config::kNumChannels; ++ch) {
        auto c     = config::get_channel(ch);
        c.protocol = led::Protocol::Off;
        config::set_channel(ch, c);
    }
    enable_control();  // Simple preset: 6 channels
    dmx::AutoPatch o;
    o.base        = 10;
    o.compact     = true;
    uint16_t next = 0;
    size_t used   = 0;
    EXPECT_TRUE(dmx::auto_patch(o, &next, &used));
    EXPECT_EQ(config::get_channel(1).universe_start, 10);
    EXPECT_EQ(config::get_channel(1).dmx_start, 151);
    EXPECT_EQ(config::get_channel(2).dmx_start, 301);
    EXPECT_EQ(config::get_control().universe, 10);  // in the room after channel 3
    EXPECT_EQ(config::get_control().address, 451);
    EXPECT_EQ(next, 11);
    EXPECT_EQ(used, 1u);
    dmx::handle_pending_remaps();
    uint8_t u[512]{};
    u[150] = 77;  // channel 1's first byte
    dmx::write_universe_from_source(10, u, sizeof(u), 1, dmx::kArtnetMergeTimeoutUs);
    dmx::note_universe_activity(10);
    EXPECT_TRUE(dmx::is_channel_active(0));
    EXPECT_TRUE(dmx::is_channel_active(1));
    EXPECT_EQ(dmx::channel_for_universe(10), 0);  // the lowest it feeds
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(1);
    EXPECT_EQ(dmx::pixel_back_buffer(1)[0], 77);
    dmx::note_universe_terminated(10);  // every output it feeds
    // Whole pixels on every channel, aligned: each opens a universe.
    o.compact = false;
    o.packing = config::kPackWholePixels;
    EXPECT_TRUE(dmx::auto_patch(o, &next));
    EXPECT_EQ(config::get_channel(1).universe_start, 11);
    EXPECT_EQ(config::get_channel(1).packing, config::kPackWholePixels);
    for (size_t ch = 0; ch < 3; ++ch) {
        config::set_channel(ch, saved[ch]);
        dmx::mark_channel_dirty(ch);
    }
    auto ctl    = config::get_control();
    ctl.enabled = 0;
    config::set_control(ctl);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
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

// ── Zones: one scene per output ─────────────────────────────────────────────

TEST(scenes_play_on_their_own_outputs_at_once) {
    const config::Scene s0 = config::get_scene(0), s1 = config::get_scene(1);
    auto a = s0, b = s1;
    a.channel_mask = 0x0F;
    b.channel_mask = 0xF0;
    config::set_scene(0, a);
    config::set_scene(1, b);
    dmx::scene_start(0);
    dmx::scene_start(1);  // a disjoint group: scene 0 keeps outputs 1-4
    EXPECT_EQ(dmx::scene_on_output(0), 0);
    EXPECT_EQ(dmx::scene_on_output(3), 0);
    EXPECT_EQ(dmx::scene_on_output(4), 1);
    EXPECT_EQ(dmx::scene_outputs(0), 0x0F);
    EXPECT_EQ(dmx::scene_outputs(1), 0xF0);
    dmx::scene_start_on(2, 0x03);  // an overlapping zone takes over outputs 1-2
    EXPECT_EQ(dmx::scene_outputs(2), 0x03);
    EXPECT_EQ(dmx::scene_outputs(0), 0x0C);
    dmx::scene_stop_scene(0);
    EXPECT_EQ(dmx::scene_on_output(2), -1);
    EXPECT_EQ(dmx::scene_on_output(0), 2);
    EXPECT_EQ(dmx::scene_on_output(5), 1);
    dmx::scene_stop();
    EXPECT_EQ(dmx::active_scene(), -1);
    config::set_scene(0, s0);
    config::set_scene(1, s1);
}

TEST(zones_follow_their_scene_through_list_edits) {
    dmx::scene_start_on(3, 0x01);
    dmx::scene_start_on(5, 0x02);
    dmx::scene_list_edited(config::SceneEdit::Delete, 3);
    EXPECT_EQ(dmx::scene_on_output(0), -1);  // its scene is gone: back to live
    EXPECT_EQ(dmx::scene_on_output(1), 4);   // shifted up with the list
}

// ── Crossfade ───────────────────────────────────────────────────────────────

TEST(scene_start_and_stop_crossfade_over_the_fade_time) {
    auto g          = config::get_global();
    g.scene_fade_ms = 1000;
    config::set_global(g);
    const uint8_t blue[12] = { 0, 0, 200, 0, 0, 200, 0, 0, 200, 0, 0, 200 };
    solid_scene(0, 255, 180, 110);
    const uint8_t* px = frame(1, blue, sizeof(blue));
    EXPECT_EQ(px[2], 200);
    dmx::scene_start(0);
    px = decode0();
    EXPECT_TRUE(px[0] < 10 && px[2] > 190);  // still the live look
    shim::advance_ms(500);
    px = decode0();
    EXPECT_TRUE(px[0] > 110 && px[0] < 145);  // half way
    shim::advance_ms(600);
    px = decode0();
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 180);
    dmx::scene_stop();  // and back, faded too
    px = decode0();
    EXPECT_TRUE(px[0] > 245);
    shim::advance_ms(1100);
    px = decode0();
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[2], 200);
    EXPECT_EQ(dmx::scene_fade_ms(), 1000u);
}

TEST(no_fade_time_switches_at_once) {
    solid_scene(0, 255, 180, 110);
    dmx::scene_start(0);
    EXPECT_EQ(decode0()[0], 255);
}

// ── Grand master, blackout, strobe ──────────────────────────────────────────

TEST(master_blackout_and_strobe_act_on_the_rendered_output) {
    const uint8_t d[12] = { 200, 100, 50, 200, 100, 50, 200, 100, 50, 200, 100, 50 };
    frame(1, d, sizeof(d));
    dmx::master_set(dmx::kAllOutputs, 32768);
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 100);
    EXPECT_EQ(px[1], 50);
    EXPECT_EQ(px[2], 25);
    dmx::blackout_set(0x02, true);  // another output: channel 1 untouched
    EXPECT_EQ(decode0()[0], 100);
    dmx::blackout_toggle(0x01);
    EXPECT_EQ(dmx::blackout_local(), 0x03);
    EXPECT_EQ(decode0()[0], 0);
    dmx::blackout_set(dmx::kAllOutputs, false);
    dmx::master_set(dmx::kAllOutputs, dmx::kMasterFull);
    dmx::strobe_set(dmx::kAllOutputs, 100);  // 10 Hz
    align_ms(100);
    EXPECT_EQ(decode0()[0], 200);  // flash
    shim::advance_ms(40);
    EXPECT_EQ(decode0()[0], 0);  // between flashes
    shim::advance_ms(60);
    EXPECT_EQ(decode0()[0], 200);
}

TEST(identify_stays_visible_through_a_blackout) {
    dmx::blackout_set(dmx::kAllOutputs, true);
    dmx::identify_start(0);
    uint8_t seen = 0;
    for (int i = 0; i < 4; ++i, shim::advance_ms(250))
        seen = static_cast<uint8_t>(seen | decode0()[0]);
    EXPECT_EQ(seen, 255);
    dmx::identify_stop();
}

// ── DMX control universe ────────────────────────────────────────────────────
// Simple preset from address 1: 1-2 master (16-bit), 3 blackout, 4 strobe,
// 5 scene, 6 fade.

TEST(control_universe_drives_master_blackout_and_scenes) {
    enable_control();
    EXPECT_EQ(dmx::control_universe(), kCtrlUni);
    EXPECT_EQ(dmx::channel_for_universe(kCtrlUni), -1);  // feeds no output
    EXPECT_FALSE(dmx::control_live());
    uint8_t u[6] = { 0x80, 0x00, 0, 0, 8, 0 };  // master half, scene band 1
    ctrl_frame(u, sizeof(u));
    EXPECT_TRUE(dmx::control_live());
    EXPECT_EQ(dmx::master_effective(0), 0x8000);
    EXPECT_EQ(dmx::master_local(0), dmx::kMasterFull);  // the desk multiplies
    EXPECT_EQ(dmx::scene_on_output(0), 0);

    dmx::scene_stop();  // a local action...
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::scene_on_output(0), -1);  // ...holds while the band does not move
    u[4] = 16;
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::scene_on_output(0), 1);

    u[2] = 255;
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::blackout_effective(), 0xFF);
    EXPECT_EQ(decode0()[0], 0);

    // The desk goes away: never left dark; the scene holds.
    shim::advance_ms(3100);
    dmx::update_show_control();
    EXPECT_FALSE(dmx::control_live());
    EXPECT_EQ(dmx::blackout_effective(), 0);
    EXPECT_EQ(dmx::master_effective(0), dmx::kMasterFull);
    EXPECT_EQ(dmx::scene_on_output(0), 1);
}

TEST(an_idle_desk_does_not_stop_a_local_scene) {
    dmx::scene_start(2);
    enable_control();
    const uint8_t idle[6] = { 0xFF, 0xFF, 0, 0, 0, 0 };
    ctrl_frame(idle, sizeof(idle));
    EXPECT_EQ(dmx::scene_on_output(0), 2);
    uint8_t go[6] = { 0xFF, 0xFF, 0, 0, 8, 0 };
    ctrl_frame(go, sizeof(go));
    EXPECT_EQ(dmx::scene_on_output(0), 0);
    go[4] = 0;  // back to "no scene" — a change, so it stops
    ctrl_frame(go, sizeof(go));
    EXPECT_EQ(dmx::scene_on_output(0), -1);
}

TEST(a_desk_already_on_a_scene_starts_it_on_first_contact) {
    enable_control();
    const uint8_t u[6] = { 0xFF, 0xFF, 0, 0, 24, 0 };  // band 3
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::scene_on_output(0), 2);
}

// Full preset: 1-2 master, 3 blackout, 4 strobe, 5 scene, 6 speed, 7 param,
// 8 effect, 9-11 colour 1, 12-14 colour 2, 15 fade, 16 FSEQ.
TEST(control_fade_fseq_and_scene_overrides) {
    solid_scene(0, 255, 180, 110);
    enable_control(config::ControlPreset::Full);
    uint8_t u[16]{};
    u[0] = u[1] = 0xFF;
    u[4]        = 8;   // scene 1 (solid warm white)
    u[14]       = 20;  // 2 s fade
    u[15]       = 16;  // FSEQ file 2
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::scene_fade_ms(), 2000u);
    EXPECT_EQ(dmx::take_fseq_request(), 1);
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::take_fseq_request(), dmx::kFseqNoRequest);  // no change, no request
    u[15] = 0;
    ctrl_frame(u, sizeof(u));
    EXPECT_EQ(dmx::take_fseq_request(), dmx::kFseqStopRequest);

    shim::advance_ms(2100);  // let the fade in finish
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 255);  // the scene's own colour
    u[8]  = 10;             // desk colour 1
    u[9]  = 20;
    u[10] = 30;
    ctrl_frame(u, sizeof(u));
    px = decode0();
    EXPECT_EQ(px[0], 10);
    EXPECT_EQ(px[1], 20);
    EXPECT_EQ(px[2], 30);

    // Switching the control universe off clears the desk's overrides.
    reset_show();
    shim::advance_ms(10);
    px = decode0();
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(dmx::scene_fade_ms(), 0u);
}

TEST(control_can_share_an_output_universe) {
    enable_control(config::ControlPreset::Simple, 1, 100);  // channel 0's universe, from slot 100
    EXPECT_EQ(dmx::channel_for_universe(1), 0);
    uint8_t u[120]{};
    for (int i = 0; i < 12; ++i)
        u[i] = 200;  // the strip's pixels
    u[99]  = 0;      // master coarse = 0
    u[100] = 0;
    ctrl_frame(u, sizeof(u), 1);
    EXPECT_TRUE(dmx::control_live());
    const uint8_t* px = decode0();
    EXPECT_EQ(px[0], 0);  // same data: pixels 200, master 0
    u[99]  = 0xFF;
    u[100] = 0xFF;
    ctrl_frame(u, sizeof(u), 1);
    EXPECT_EQ(decode0()[0], 200);
}

// Eight full RGBW outputs take 64 universes: the control universe still gets
// a pool slot (the 65th — the third word of the dirty mask) and is heard.
TEST(eight_full_rgbw_outputs_leave_room_for_the_control_universe) {
    config::ChannelConfig saved[config::kNumChannels];
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        saved[ch]        = config::get_channel(ch);
        auto c           = saved[ch];
        c.protocol       = led::Protocol::SK6812;
        c.pixel_count    = 1024;  // 4096 B: 8 universes
        c.universe_start = static_cast<uint16_t>(1 + ch * 8);
        c.dmx_start      = 1;
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    enable_control();
    EXPECT_EQ(dmx::control_pool_slot(), 64);
    uint8_t u[6] = { 0x40, 0x00, 0, 0, 0, 0 };  // master a quarter
    ctrl_frame(u, sizeof(u));
    EXPECT_TRUE(dmx::control_live());
    EXPECT_EQ(dmx::master_effective(0), 0x4000);
    u[0] = 0xFF;
    u[1] = 0xFF;
    ctrl_frame(u, sizeof(u));
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        config::set_channel(ch, saved[ch]);
        dmx::mark_channel_dirty(ch);
    }
    auto ctl    = config::get_control();
    ctl.enabled = 0;
    config::set_control(ctl);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
}

// ── Scenes on groups ─────────────────────────────────────────────────────────
// Outputs 1-2, 5 bars of 4 LEDs each. "Top" runs from output 1's last bar
// (left edge) to output 2's last bar (right edge); "Centre" = the two bars
// next to the middle.
namespace {
config::ChannelConfig g_saved_ch[2];
void two_outputs_of_bars() {
    for (size_t ch = 0; ch < 2; ++ch) {
        g_saved_ch[ch]     = config::get_channel(ch);
        auto c             = g_saved_ch[ch];
        c.protocol         = led::Protocol::WS2815;
        c.pixel_count      = 20;
        c.grouping         = 1;
        c.invert_direction = false;
        std::memset(c.gaps, 0, sizeof(c.gaps));
        std::memset(c.fixtures, 0, sizeof(c.fixtures));
        for (uint16_t k = 0; k < 5; ++k)
            c.fixtures[k] = { static_cast<uint16_t>(k * 4), 4 };
        c.universe_start = static_cast<uint16_t>(200 + ch);
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    dmx::handle_pending_remaps();
    static config::GroupsConfig g{};
    g       = config::GroupsConfig{};
    g.count = 2;
    std::strcpy(g.groups[0].name, "Top");
    g.groups[0].count = 10;
    for (uint8_t k = 0; k < 5; ++k) {
        g.groups[0].members[k]     = { 0, static_cast<uint8_t>(4 - k) };
        g.groups[0].members[5 + k] = { 1, k };
    }
    std::strcpy(g.groups[1].name, "Centre");
    g.groups[1].count      = 2;
    g.groups[1].members[0] = { 0, 0 };
    g.groups[1].members[1] = { 1, 0 };
    config::set_groups(g);
}
const uint8_t* frame(size_t ch) {
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(ch);
    return dmx::pixel_back_buffer(ch);
}
}  // namespace

TEST(a_scene_on_a_group_spans_outputs_and_yields_to_a_smaller_one) {
    reset_show();
    dmx::scene_stop_on(dmx::kAllOutputs, 0);  // nothing left on the outputs
    auto gl          = config::get_global();
    gl.failsafe_mode = config::kFailsafeHold;  // live = what arrived: nothing here
    config::set_global(gl);
    two_outputs_of_bars();
    solid_scene(0, 200, 0, 0);
    solid_scene(1, 0, 0, 150);
    const uint8_t live0 = frame(0)[0], live1 = frame(1)[19 * 3];  // whatever the banks hold
    dmx::group_play(0, 0, 0);  // red on Top, no fade
    EXPECT_EQ(frame(0)[0], 200);
    EXPECT_EQ(frame(1)[19 * 3], 200);
    dmx::group_play(1, 1, 0);  // blue on Centre: those two bars only
    EXPECT_EQ(dmx::fixture_scene(0, 0), 1);
    EXPECT_EQ(dmx::fixture_scene(0, 1), 0);
    const uint8_t* o1 = frame(0);
    EXPECT_EQ(o1[0 * 3 + 2], 150);  // bar 1 of output 1: blue
    EXPECT_EQ(o1[4 * 3], 200);      // bar 2: still red
    dmx::PlayInfo plays[4];
    EXPECT_EQ(dmx::active_plays(plays, 4), 2u);
    dmx::group_stop(1, 0);  // Centre stops: its bars go back to their output (live)
    EXPECT_EQ(dmx::fixture_scene(0, 0), -1);
    EXPECT_EQ(frame(0)[0], live0);
    dmx::scene_start_on(1, 0x01, 0);  // output 1 whole: takes its Top bars back
    EXPECT_EQ(dmx::fixture_scene(0, 3), -1);
    EXPECT_EQ(dmx::fixture_scene(1, 3), 0);  // output 2 keeps Top
    EXPECT_EQ(frame(0)[8 * 3 + 2], 150);
    dmx::scene_stop_on(dmx::kAllOutputs, 0);  // everything back to live
    EXPECT_EQ(dmx::active_plays(plays, 4), 0u);
    EXPECT_EQ(frame(1)[19 * 3], live1);
}

// Chained along Top: the strip continues from output 1's left bar to output
// 2's right bar, whatever the wiring; reversed it runs from the right edge.
TEST(a_chained_scene_runs_along_the_group_order) {
    reset_show();
    two_outputs_of_bars();
    config::Scene sc{};
    sc.effect       = config::kSceneFxGradient;
    sc.num_colors   = 2;
    sc.channel_mask = 0xFF;
    config::set_scene_color(sc, 0, 255, 0, 0);
    config::set_scene_color(sc, 1, 0, 0, 255);
    sc.param        = 1;
    sc.fixture_mode = config::pack_scene_mode(config::kFixtureModeChain, false, 0);
    config::set_scene(2, sc);
    uint8_t ref[40 * 3];
    dmx::scene_start(2);  // its default group: Top
    EXPECT_EQ(dmx::fixture_scene(1, 4), 2);
    const uint8_t* a = frame(0);
    uint8_t out1[20 * 3];
    std::memcpy(out1, a, sizeof(out1));
    const uint8_t* b = frame(1);
    // Member 1 (output 1, bar 5 = LEDs 16-19) holds the strip's start, member
    // 10 (output 2, bar 5) its end: they differ, and member 5 (output 1, bar
    // 1) meets member 6 (output 2, bar 1) in the middle.
    EXPECT_TRUE(std::memcmp(out1 + 16 * 3, b + 16 * 3, 3) != 0);
    (void)ref;
    dmx::scene_stop();
}

// ── Accessors and corners ───────────────────────────────────────────────────

TEST(accessors_report_the_current_state) {
    dmx::set_pixel_preview(0, 33);
    EXPECT_EQ(dmx::pixel_preview_count(), 33);
    dmx::clear_pixel_preview();
    auto c    = config::get_channel(0);
    c.gaps[0] = { 1, 2 };
    config::set_channel(0, c);
    EXPECT_EQ(dmx::channel_gap_count(0), 1u);
    EXPECT_EQ(dmx::channel_gap_count(99), 0u);
    EXPECT_EQ(dmx::physical_pixels(config::get_channel(0)), 6u);  // 4 live + 2 dead
    one_channel();
    dmx::fseq_set_active(true);
    EXPECT_TRUE(dmx::fseq_is_active());
    const uint8_t d[3] = { 9, 8, 7 };
    EXPECT_EQ(frame(1, d, sizeof(d))[0], 9);  // FSEQ live path decodes the banks
    dmx::fseq_set_active(false);
    const auto u0 = dmx::get_stats().dma_underruns;
    dmx::note_dma_underrun();
    EXPECT_EQ(dmx::get_stats().dma_underruns, u0 + 1);
    EXPECT_EQ(dmx::channel_for_universe(40000), -1);  // past the 15-bit range
}

TEST(an_exhausted_pool_is_reported_not_overrun) {
    // 8 × 1024 px RGBW = 8 universes each: 64 of the 72 slots.
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = led::Protocol::SK6812;
        c.pixel_count    = 1024;
        c.universe_start = static_cast<uint16_t>(1 + ch * 8);
        config::set_channel(ch, c);
    }
    apply_channels();
    enable_control();  // the pool keeps room for it
    EXPECT_EQ(dmx::control_pool_slot(), 64);
    auto top           = config::get_channel(7);
    top.universe_start = 0x7FFE;  // runs past the top of the range: partly unmapped
    config::set_channel(7, top);
    apply_channels();
    EXPECT_EQ(dmx::channel_for_universe(0x7FFE), 7);
    EXPECT_EQ(dmx::channel_for_universe(0x7FFF), 7);
    reset_show();
    one_channel();
}

// The dashboard preview reads the front buffer: averaged buckets, W folded
// into every colour, nothing for an Off output.
TEST(output_preview_shrinks_the_front_buffer_to_rgb) {
    one_channel(4);
    const uint8_t d[12] = { 10, 20, 30, 30, 40, 50, 200, 0, 0, 100, 0, 0 };
    frame(1, d, sizeof(d));
    dmx::swap_pixels(0);
    uint8_t rgb[12]{};
    EXPECT_EQ(dmx::output_preview(0, rgb, 4), 4u);
    EXPECT_TRUE(std::memcmp(rgb, d, 12) == 0);
    EXPECT_EQ(dmx::output_preview(0, rgb, 2), 2u);
    const uint8_t halves[6] = { 20, 30, 40, 150, 0, 0 };
    EXPECT_TRUE(std::memcmp(rgb, halves, 6) == 0);
    EXPECT_EQ(dmx::output_preview(1, rgb, 4), 0u);  // Off

    const uint8_t rgbw[8] = { 10, 250, 0, 20, 0, 0, 0, 0 };
    uint8_t out[3];
    EXPECT_EQ(dmx::logic::downsample_rgb(rgbw, 1, 4, out, 1), 1u);
    EXPECT_EQ(out[0], 30);
    EXPECT_EQ(out[1], 255);  // clipped
    EXPECT_EQ(out[2], 20);
}
