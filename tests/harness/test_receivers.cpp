// artnet.cpp + sacn.cpp — the real receive loops, fed through the in-process
// datagram fabric, all the way to the decoded pixel buffer (real dmx_manager
// and config_store underneath). FSEQ is a controllable fake.

#include <cstring>
#include <vector>

#include "artnet.h"
#include "artnet_parser.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "harness.h"
#include "sacn.h"
#include "sacn_parser.h"
#include "shim_control.h"

using namespace pixfrog;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint16_t kArt = artnet::kArtnetPort;
std::vector<uint32_t> g_sacn_groups;  // multicast groups joined, sampled before stop
constexpr uint16_t kSacn = sacn::parser::kSacnPort;

Bytes art_header(uint16_t op, size_t size) {
    Bytes p(size, 0);
    std::memcpy(p.data(), "Art-Net\0", 8);
    p[8]  = static_cast<uint8_t>(op & 0xFF);
    p[9]  = static_cast<uint8_t>(op >> 8);
    p[11] = 14;
    return p;
}

Bytes art_dmx(uint16_t universe, const Bytes& data) {
    Bytes p = art_header(artnet::parser::kOpDmx, 18 + data.size());
    p[14]   = static_cast<uint8_t>(universe & 0xFF);
    p[15]   = static_cast<uint8_t>(universe >> 8);
    p[16]   = static_cast<uint8_t>(data.size() >> 8);
    p[17]   = static_cast<uint8_t>(data.size() & 0xFF);
    std::memcpy(p.data() + 18, data.data(), data.size());
    return p;
}

Bytes sacn_data(uint16_t universe, uint8_t priority, const Bytes& slots, uint8_t options = 0,
                uint8_t cid0 = 0, uint8_t start_code = 0) {
    Bytes p(126 + slots.size(), 0);
    p[1] = 0x10;
    std::memcpy(p.data() + 4, sacn::parser::kAcnId, sizeof(sacn::parser::kAcnId));
    p[21] = 0x04;
    for (int i = 0; i < 16; ++i)
        p[22 + i] = static_cast<uint8_t>(i + cid0);
    p[43]                = 0x02;
    p[108]               = priority;
    p[111]               = 1;
    p[112]               = options;
    p[113]               = static_cast<uint8_t>(universe >> 8);
    p[114]               = static_cast<uint8_t>(universe & 0xFF);
    p[117]               = 0x02;
    p[118]               = 0xA1;
    p[122]               = 0x01;
    const uint16_t props = static_cast<uint16_t>(1 + slots.size());
    p[123]               = static_cast<uint8_t>(props >> 8);
    p[124]               = static_cast<uint8_t>(props & 0xFF);
    p[125]               = start_code;
    std::memcpy(p.data() + 126, slots.data(), slots.size());
    return p;
}

// Run the receiver task until its queue drains (the idle hook stops it).
void pump_artnet() {
    artnet::start();
    shim::run_task("artnet_rx");
}
void pump_sacn() {
    sacn::start();
    shim::run_task("sacn_rx");
}

const uint8_t* pixels0() {
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
    shim::net_reset();
    shim::tasks_forget();
    shim::net_on_idle(kArt, [] { artnet::stop(); });
    shim::net_on_idle(kSacn, [] {
        g_sacn_groups = shim::net_groups(kSacn);
        sacn::stop();
    });
    artnet::set_local_ip(0xC0A80232);  // 192.168.2.50
    fseq::fake::set(fseq::Status::Idle, 0);
    dmx::sync_reset();
    dmx::scene_stop();
    dmx::merge_cancel_all();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch == 0 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = 4;
        c.universe_start = static_cast<uint16_t>(1 + ch * 8);
        c.dmx_start      = 1;
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    auto g                      = config::get_global();
    g.artnet_poll_reply_unicast = false;
    g.merge_mode                = config::kMergeHtp;
    config::set_global(g);
    dmx::handle_pending_remaps();
    shim::advance_ms(1000);
}

}  // namespace

TEST(artdmx_reaches_pixels_and_counts) {
    const auto before = dmx::get_stats();
    shim::net_push(kArt, art_dmx(1, { 11, 22, 33 }));
    shim::net_push(kArt, art_dmx(300, { 1, 2, 3 }));  // not ours: silently dropped
    shim::net_push(kArt, { 'x', 'y' });               // not Art-Net: counted bad
    pump_artnet();
    EXPECT_EQ(pixels0()[1], 22);
    const auto after = dmx::get_stats();
    EXPECT_EQ(after.artnet_packets_rx, before.artnet_packets_rx + 1);
    EXPECT_EQ(after.artnet_bad_packets, before.artnet_bad_packets + 1);
    EXPECT_TRUE(dmx::is_channel_active(0));
}

TEST(two_senders_merge_htp_third_dropped) {
    shim::net_push(kArt, art_dmx(1, { 100, 0, 0 }), 0x0A000001);
    shim::net_push(kArt, art_dmx(1, { 0, 90, 0 }), 0x0A000002);
    shim::net_push(kArt, art_dmx(1, { 255, 255, 255 }), 0x0A000003);
    pump_artnet();
    const uint8_t* px = pixels0();
    EXPECT_EQ(px[0], 100);
    EXPECT_EQ(px[1], 90);
    EXPECT_EQ(px[2], 0);
}

TEST(artpoll_answers_two_bind_replies) {
    shim::net_push(kArt, art_header(artnet::parser::kOpPoll, 14));
    pump_artnet();
    auto& sent = shim::net_sent();
    EXPECT_EQ(sent.size(), 2);  // bind 1 = channels 1-4, bind 2 = 5-8
    if (sent.size() == 2) {
        EXPECT_EQ(sent[0].ip, 0xFFFFFFFFu);  // broadcast by default
        EXPECT_EQ(sent[0].bytes.size(), artnet::parser::kPollReplySize);
        EXPECT_EQ(sent[0].bytes[9], 0x21);  // OpPollReply
    }
}

TEST(artpoll_unicast_and_silent_without_ip) {
    auto g                      = config::get_global();
    g.artnet_poll_reply_unicast = true;
    config::set_global(g);
    shim::net_push(kArt, art_header(artnet::parser::kOpPoll, 14), 0x0A0000FE);
    pump_artnet();
    EXPECT_EQ(shim::net_sent().size(), 2);
    if (!shim::net_sent().empty()) EXPECT_EQ(shim::net_sent()[0].ip, 0x0A0000FEu);
    shim::net_sent().clear();
    artnet::set_local_ip(0);  // no address yet: nothing to advertise
    shim::net_push(kArt, art_header(artnet::parser::kOpPoll, 14));
    pump_artnet();
    EXPECT_EQ(shim::net_sent().size(), 0);
}

TEST(artaddress_programs_names_universe_and_merge) {
    Bytes p = art_header(artnet::parser::kOpAddress, artnet::parser::kAddressSize);
    std::memcpy(p.data() + 14, "desk-name", 9);
    p[13]  = 1;     // bind 1 → channels 1-4
    p[100] = 0x85;  // port 0 (channel 1): universe low nibble 5
    p[106] = 0x11;  // AcMergeLtp0
    shim::net_push(kArt, p);
    pump_artnet();
    EXPECT_STREQ(config::get_global().short_name, "desk-name");
    EXPECT_EQ(config::get_channel(0).universe_start & 0x0F, 5);
    EXPECT_EQ(config::get_global().merge_mode, config::kMergeLtp);
    EXPECT_EQ(shim::net_sent().size(), 2);  // answered with ArtPollReply
    auto g       = config::get_global();
    g.merge_mode = config::kMergeHtp;
    std::strcpy(g.short_name, "pixfrog");
    config::set_global(g);
}

TEST(artipprog_programs_static_ip_and_replies) {
    Bytes p = art_header(artnet::parser::kOpIpProg, 34);
    p[14]   = 0x80 | 0x04 | 0x02;  // enable, program IP + mask
    p[16] = 10, p[17] = 1, p[18] = 2, p[19] = 3;
    p[20] = 255, p[21] = 0, p[22] = 0, p[23] = 0;
    shim::net_push(kArt, p, 0x0A0000FE);
    pump_artnet();
    EXPECT_FALSE(config::get_global().use_dhcp);
    EXPECT_EQ(config::get_global().static_ip, 0x0A010203u);
    EXPECT_EQ(config::get_global().static_mask, 0xFF000000u);
    EXPECT_EQ(shim::net_sent().size(), 1);
    if (!shim::net_sent().empty()) EXPECT_EQ(shim::net_sent()[0].bytes[9], 0xF9);
}

TEST(arttrigger_plays_and_stops_scenes) {
    auto trig = [](uint8_t sub) {
        Bytes p = art_header(artnet::parser::kOpTrigger, 18 + 512);
        p[14] = 0xFF, p[15] = 0xFF;  // global OEM
        p[16] = 3;                   // KeyShow
        p[17] = sub;
        return p;
    };
    shim::net_push(kArt, trig(2));
    pump_artnet();
    EXPECT_EQ(dmx::active_scene(), 1);
    shim::net_push(kArt, trig(200));  // past the list: ignored
    pump_artnet();
    EXPECT_EQ(dmx::active_scene(), 1);
    shim::net_push(kArt, trig(0));
    pump_artnet();
    EXPECT_EQ(dmx::active_scene(), -1);
}

TEST(arttimecode_resyncs_a_drifting_fseq_only) {
    auto tc = [](uint8_t s) {
        Bytes p = art_header(artnet::parser::kOpTimeCode, artnet::parser::kTimeCodeSize);
        p[15]   = s;  // seconds
        p[18]   = 3;  // SMPTE 30 fps
        return p;
    };
    fseq::fake::set(fseq::Status::Playing, 10'050);
    shim::net_push(kArt, tc(10));  // 50 ms off: tolerated
    pump_artnet();
    EXPECT_EQ(fseq::fake::seeks().size(), 0);
    shim::net_push(kArt, tc(20));  // 10 s off: seek
    pump_artnet();
    EXPECT_EQ(fseq::fake::seeks().size(), 1);
    if (!fseq::fake::seeks().empty()) EXPECT_EQ(fseq::fake::seeks()[0], 20'000);
    fseq::fake::set(fseq::Status::Idle, 0);
    shim::net_push(kArt, tc(40));  // not playing: never auto-starts
    pump_artnet();
    EXPECT_EQ(fseq::fake::seeks().size(), 0);
}

TEST(artsync_wakes_the_render_loop) {
    dmx::wait_for_sync_or_period(0);  // drain any pending give
    shim::net_push(kArt, art_header(artnet::parser::kOpSync, 14));
    pump_artnet();
    EXPECT_TRUE(dmx::wait_for_sync_or_period(5));
}

TEST(sacn_data_reaches_pixels_and_joins_groups) {
    shim::net_push(kSacn, sacn_data(1, 100, { 7, 8, 9 }));
    pump_sacn();
    EXPECT_EQ(pixels0()[2], 9);
    // Only channel 0 is on: 4 px = one universe → one group, 239.255.0.1.
    EXPECT_EQ(g_sacn_groups.size(), 1);
    if (!g_sacn_groups.empty()) EXPECT_EQ(g_sacn_groups[0], 0xEFFF0001u);
}

TEST(sacn_preview_nonzero_startcode_and_foreign_universes_ignored) {
    shim::net_push(kSacn, sacn_data(1, 100, { 1, 1, 1 }));
    shim::net_push(kSacn, sacn_data(1, 100, { 2, 2, 2 }, sacn::parser::kOptPreview));
    shim::net_push(kSacn, sacn_data(1, 100, { 3, 3, 3 }, 0, 0, 0xDD));
    shim::net_push(kSacn, sacn_data(40000, 100, { 4, 4, 4 }));
    pump_sacn();
    EXPECT_EQ(pixels0()[0], 1);
}

TEST(sacn_priority_gate_keeps_the_higher_source) {
    shim::net_push(kSacn, sacn_data(1, 150, { 50, 50, 50 }, 0, 0));
    shim::net_push(kSacn, sacn_data(1, 100, { 99, 99, 99 }, 0, 0x40));
    pump_sacn();
    EXPECT_EQ(pixels0()[0], 50);
    shim::advance_ms(3000);  // the high-priority source went silent
    shim::net_push(kSacn, sacn_data(1, 100, { 99, 99, 99 }, 0, 0x40));
    pump_sacn();
    EXPECT_EQ(pixels0()[0], 99);
}

TEST(sacn_stream_terminated_triggers_failsafe_now) {
    auto g               = config::get_global();
    g.failsafe_mode      = config::kFailsafeBlackout;
    g.failsafe_timeout_s = 30;
    config::set_global(g);
    shim::net_push(kSacn, sacn_data(1, 100, { 70, 70, 70 }));
    pump_sacn();
    EXPECT_EQ(pixels0()[0], 70);
    shim::net_push(kSacn, sacn_data(1, 100, { 70, 70, 70 }, sacn::parser::kOptTerminated));
    pump_sacn();
    EXPECT_TRUE(dmx::is_channel_failsafe(0));
    EXPECT_EQ(pixels0()[0], 0);
    g.failsafe_mode = config::kFailsafeHold;
    config::set_global(g);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}

// ── Show control from the network ───────────────────────────────────────────

namespace {
void control_on(uint16_t universe) {
    auto c     = config::default_control();  // simple: master16, blackout, strobe, scene, fade
    c.enabled  = 1;
    c.universe = universe;
    config::set_control(c);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
}
void control_off() {
    config::set_control(config::default_control());
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    dmx::update_show_control();
    dmx::blackout_set(dmx::kAllOutputs, false);
}
}  // namespace

TEST(artdmx_on_the_control_universe_drives_the_show) {
    control_on(200);
    const auto before = dmx::get_stats().artnet_packets_rx;
    shim::net_push(kArt, art_dmx(200, { 0x40, 0x00, 255, 0, 0, 0 }));
    pump_artnet();
    dmx::swap_universes();
    dmx::update_show_control();
    EXPECT_TRUE(dmx::control_live());
    EXPECT_EQ(dmx::master_effective(0), 0x4000);
    EXPECT_EQ(dmx::blackout_effective(), 0xFF);
    EXPECT_EQ(dmx::get_stats().artnet_packets_rx, before + 1);  // counted like any universe
    control_off();
}

TEST(arttrigger_macro_key_drives_the_blackout) {
    auto macro = [](uint8_t sub) {
        Bytes p = art_header(artnet::parser::kOpTrigger, 18 + 512);
        p[14] = 0xFF, p[15] = 0xFF;
        p[16] = 1;  // KeyMacro
        p[17] = sub;
        return p;
    };
    shim::net_push(kArt, macro(2));
    pump_artnet();
    EXPECT_EQ(dmx::blackout_local(), 0xFF);
    shim::net_push(kArt, macro(1));  // toggle
    pump_artnet();
    EXPECT_EQ(dmx::blackout_local(), 0);
    shim::net_push(kArt, macro(1));
    shim::net_push(kArt, macro(3));  // off
    pump_artnet();
    EXPECT_EQ(dmx::blackout_local(), 0);
}

TEST(sacn_joins_the_control_universe_group) {
    auto g         = config::get_global();
    g.sacn_enabled = true;
    config::set_global(g);
    control_on(300);
    pump_sacn();
    bool joined = false;
    for (uint32_t grp : g_sacn_groups)
        if (grp == (0xEFFF0000u | 300u)) joined = true;
    EXPECT_TRUE(joined);
    shim::net_push(kSacn, sacn_data(300, 100, { 0xFF, 0xFF, 0, 0, 16, 0 }));
    pump_sacn();
    dmx::swap_universes();
    dmx::update_show_control();
    EXPECT_EQ(dmx::scene_on_output(0), 1);  // band 2
    dmx::scene_stop();
    control_off();
}

// ── Malformed packets, remote programming corners, socket failures ──────────

TEST(malformed_artnet_packets_are_counted_bad) {
    const auto before = dmx::get_stats().artnet_bad_packets;
    shim::net_push(kArt, art_header(artnet::parser::kOpDmx, 12));      // no DMX header
    shim::net_push(kArt, art_header(artnet::parser::kOpPoll, 12));     // short poll
    shim::net_push(kArt, art_header(artnet::parser::kOpAddress, 40));  // short ArtAddress
    shim::net_push(kArt, art_header(artnet::parser::kOpIpProg, 20));   // short ArtIpProg
    shim::net_push(kArt, art_header(artnet::parser::kOpNzs, 12));      // short ArtNzs
    shim::net_push(kArt, art_header(artnet::parser::kOpTrigger, 12));  // short ArtTrigger
    shim::net_push(kArt, art_header(artnet::parser::kOpTimeCode, 12));
    shim::net_push(kArt, art_header(artnet::parser::kOpCommand, 12));  // short ArtCommand
    pump_artnet();
    EXPECT_EQ(dmx::get_stats().artnet_bad_packets, before + 8);
}

TEST(artnzs_and_artcommand_are_counted_control) {
    const auto before = dmx::get_stats().artnet_ctrl_rx;
    Bytes nzs         = art_dmx(1, { 1, 2, 3 });
    nzs[8]            = 0x00;
    nzs[9]            = 0x51;  // OpNzs
    nzs[13]           = 0xDD;  // start code
    shim::net_push(kArt, nzs);
    shim::net_push(kArt, art_header(artnet::parser::kOpCommand, 16 + 8));
    shim::net_push(kArt, art_header(0x1234, 20));  // unknown opcode: ignored
    pump_artnet();
    EXPECT_EQ(dmx::get_stats().artnet_ctrl_rx, before + 2);
}

TEST(artaddress_switches_long_name_and_commands) {
    Bytes p = art_header(artnet::parser::kOpAddress, artnet::parser::kAddressSize);
    p[12]   = 0x80 | 3;  // program net 3
    p[104]  = 0x80 | 7;  // program subnet 7
    std::memcpy(p.data() + 32, "A long desk name", 16);
    p[106] = artnet::parser::kAcCancelMerge;
    shim::net_push(kArt, p);
    Bytes q = art_header(artnet::parser::kOpAddress, artnet::parser::kAddressSize);
    q[106]  = 0x90;  // a command we do not implement: logged, nothing changes
    shim::net_push(kArt, q);
    pump_artnet();
    EXPECT_EQ(config::get_global().artnet_net, 3);
    EXPECT_EQ(config::get_global().artnet_subnet, 7);
    EXPECT_STREQ(config::get_global().long_name, "A long desk name");
    auto g          = config::get_global();
    g.artnet_net    = 0;
    g.artnet_subnet = 0;
    config::set_global(g);
}

TEST(artipprog_reset_to_defaults_and_dhcp) {
    Bytes p = art_header(artnet::parser::kOpIpProg, 34);
    p[14]   = 0x80 | 0x10;  // enable + reset to defaults
    shim::net_push(kArt, p);
    pump_artnet();
    EXPECT_TRUE(config::get_global().use_dhcp);
    EXPECT_EQ(config::get_global().static_ip, 0u);
    p[14] = 0x80 | 0x40 | 0x08;                   // DHCP on, program the gateway
    p[26] = 10, p[27] = 0, p[28] = 0, p[29] = 1;  // ProgDg (24-25 = the deprecated port)
    shim::net_push(kArt, p);
    p[14] = 0x00;  // programming disabled: reply only
    shim::net_push(kArt, p);
    pump_artnet();
    EXPECT_EQ(config::get_global().static_gateway, 0x0A000001u);
    EXPECT_EQ(shim::net_sent().size(), 3);  // one ArtIpProgReply each
}

TEST(artpollreply_send_failure_is_survived) {
    shim::fail_next(shim::Fault::SendTo, 5);
    shim::net_push(kArt, art_header(artnet::parser::kOpPoll, 14));
    pump_artnet();
    EXPECT_EQ(shim::net_sent().size(), 0);
    shim::faults_clear();
}

TEST(artnet_socket_and_bind_failures_end_the_task) {
    shim::fail_next(shim::Fault::Socket);
    artnet::start();
    shim::run_task("artnet_rx");  // returns at once: no socket
    artnet::stop();
    shim::fail_next(shim::Fault::Bind);
    artnet::start();
    shim::run_task("artnet_rx");
    artnet::stop();
    shim::faults_clear();
    shim::net_push(kArt, art_dmx(1, { 5, 6, 7 }));  // a later start still works
    pump_artnet();
    EXPECT_EQ(pixels0()[0], 5);
}

TEST(sacn_leaves_stale_groups_and_survives_join_failure) {
    auto g         = config::get_global();
    g.sacn_enabled = true;
    config::set_global(g);
    pump_sacn();
    EXPECT_EQ(g_sacn_groups.size(), 1);  // universe 1
    auto c           = config::get_channel(0);
    c.universe_start = 5;  // moves: 1 must be left, 5 joined
    config::set_channel(0, c);
    dmx::mark_channel_dirty(0);
    dmx::handle_pending_remaps();
    shim::fail_next(shim::Fault::Join);  // the first join fails, is retried later
    shim::advance_ms(6000);              // past the membership refresh period
    shim::net_push(kSacn, sacn_data(5, 100, { 1, 1, 1 }));
    pump_sacn();
    c.universe_start = 1;
    config::set_channel(0, c);
    dmx::mark_channel_dirty(0);
    dmx::handle_pending_remaps();
    shim::faults_clear();
}

TEST(sacn_sync_and_malformed_packets) {
    const auto bad = dmx::get_stats().artnet_bad_packets;
    Bytes sync(49, 0);
    sync[1] = 0x10;
    std::memcpy(sync.data() + 4, sacn::parser::kAcnId, sizeof(sacn::parser::kAcnId));
    sync[21] = 0x08;  // root vector: extended
    sync[43] = 0x01;  // framing vector: sync
    shim::net_push(kSacn, sync);
    Bytes junk = sacn_data(1, 100, { 1 });
    junk[21]   = 0x04;
    junk[43]   = 0x7F;  // data root, bad framing vector
    shim::net_push(kSacn, junk);
    Bytes other = sacn_data(1, 100, { 1 });
    other[21]   = 0x09;  // unknown root vector
    shim::net_push(kSacn, other);
    pump_sacn();
    EXPECT_TRUE(dmx::get_stats().artnet_bad_packets >= bad + 2);
}

TEST(sacn_socket_and_bind_failures_end_the_task) {
    shim::fail_next(shim::Fault::Socket);
    sacn::start();
    shim::run_task("sacn_rx");
    sacn::stop();
    shim::fail_next(shim::Fault::Bind);
    sacn::start();
    shim::run_task("sacn_rx");
    sacn::stop();
    shim::faults_clear();
    EXPECT_FALSE(sacn::is_running());
}

// ── Sync mode: ArtSync / E1.31 synchronization ──────────────────────────────

namespace {
// Channel 0 spans universes 1 and 2 (200 px: 170 in U1, 30 in U2).
void two_universe_channel() {
    auto c        = config::get_channel(0);
    c.pixel_count = 200;
    config::set_channel(0, c);
    dmx::mark_channel_dirty(0);
    dmx::handle_pending_remaps();
}
uint8_t u1_px() {
    return pixels0()[0];
}
uint8_t u2_px() {
    // Universe 2's first slot: the channel runs on through all 512 slots of
    // universe 1. Decoded by the u1_px() call before it.
    return dmx::pixel_back_buffer(0)[512];
}
Bytes sacn_synced(uint16_t universe, const Bytes& slots, uint16_t sync_addr, uint8_t options = 0) {
    Bytes p = sacn_data(universe, 100, slots, options);
    p[109]  = static_cast<uint8_t>(sync_addr >> 8);
    p[110]  = static_cast<uint8_t>(sync_addr & 0xFF);
    return p;
}
Bytes sacn_sync(uint16_t sync_addr) {
    Bytes p(49, 0);
    p[1] = 0x10;
    std::memcpy(p.data() + 4, sacn::parser::kAcnId, sizeof(sacn::parser::kAcnId));
    p[21] = 0x08;  // root vector: extended
    p[43] = 0x01;  // framing vector: sync
    p[45] = static_cast<uint8_t>(sync_addr >> 8);
    p[46] = static_cast<uint8_t>(sync_addr & 0xFF);
    return p;
}
}  // namespace

// Before: the first universe that arrived was published alone, so a channel
// spanning two universes showed two source frames at once.
TEST(artsync_publishes_every_universe_of_a_frame_together) {
    two_universe_channel();
    shim::net_push(kArt, art_dmx(1, { 10 }));
    shim::net_push(kArt, art_dmx(2, { 20 }));
    shim::net_push(kArt, art_header(artnet::parser::kOpSync, 14));
    pump_artnet();
    EXPECT_EQ(u1_px(), 10);
    EXPECT_EQ(u2_px(), 20);
    EXPECT_TRUE(dmx::sync_mode());
    shim::net_push(kArt, art_dmx(1, { 11 }));  // half of the next frame…
    pump_artnet();
    EXPECT_EQ(u1_px(), 10);  // …waits for its sync
    shim::net_push(kArt, art_dmx(2, { 21 }));
    shim::net_push(kArt, art_header(artnet::parser::kOpSync, 14));
    pump_artnet();
    EXPECT_EQ(u1_px(), 11);
    EXPECT_EQ(u2_px(), 21);
    // A sync with nothing new is used up: the next data waits for its own.
    shim::net_push(kArt, art_header(artnet::parser::kOpSync, 14));
    pump_artnet();
    EXPECT_EQ(u1_px(), 11);
    shim::net_push(kArt, art_dmx(1, { 12 }));
    pump_artnet();
    EXPECT_EQ(u1_px(), 11);
    // Syncs stop for 4 s: free-run again (Art-Net 4).
    shim::advance_ms(dmx::kArtSyncTimeoutMs + 1);
    EXPECT_FALSE(dmx::sync_mode());
    EXPECT_EQ(u1_px(), 12);
    shim::net_push(kArt, art_dmx(1, { 13 }));
    pump_artnet();
    EXPECT_EQ(u1_px(), 13);
}

TEST(sacn_data_waits_for_a_sync_on_its_own_address) {
    two_universe_channel();
    shim::net_push(kSacn, sacn_data(1, 100, { 1 }));  // unsynchronized: published at once
    pump_sacn();
    EXPECT_EQ(u1_px(), 1);
    shim::net_push(kSacn, sacn_synced(1, { 30 }, 7));
    shim::net_push(kSacn, sacn_synced(2, { 40 }, 7));
    shim::net_push(kSacn, sacn_sync(8));  // another address: not ours
    pump_sacn();
    EXPECT_TRUE(dmx::sync_mode());
    EXPECT_EQ(u1_px(), 1);
    shim::net_push(kSacn, sacn_sync(7));
    pump_sacn();
    EXPECT_EQ(u1_px(), 30);
    EXPECT_EQ(u2_px(), 40);
    // No sync for 2.5 s: back to free-run, unless Force_Synchronization asks
    // to keep waiting.
    shim::net_push(kSacn, sacn_synced(1, { 31 }, 7, sacn::parser::kOptForceSync));
    pump_sacn();
    shim::advance_ms(dmx::kE131SyncTimeoutMs + 1);
    EXPECT_TRUE(dmx::sync_mode());
    EXPECT_EQ(u1_px(), 30);
    shim::net_push(kSacn, sacn_data(1, 100, { 32 }));  // the source goes unsynchronized
    pump_sacn();
    shim::advance_ms(dmx::kE131SyncTimeoutMs + 1);
    EXPECT_FALSE(dmx::sync_mode());
    EXPECT_EQ(u1_px(), 32);
    Bytes short_sync = sacn_sync(7);
    short_sync.resize(40);  // malformed: counted, ignored
    const auto bad = dmx::get_stats().artnet_bad_packets;
    shim::net_push(kSacn, short_sync);
    pump_sacn();
    EXPECT_EQ(dmx::get_stats().artnet_bad_packets, bad + 1);
}
