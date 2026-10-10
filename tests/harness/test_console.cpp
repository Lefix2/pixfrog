// control_console.cpp — every command through the real parser/handlers, as
// typed on the UART (shim::console_exec), against the real config_store,
// dmx_manager and sacn; hardware-facing modules are fakes.

#include <cstring>
#include <string>
#include <vector>

#include "config_store.h"
#include "control_console.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
#include "fakes/net_fake.h"
#include "harness.h"
#include "shim_control.h"

using namespace pixfrog;

namespace {

std::string g_out;

// Runs `line`; true when the reply ends in OK (not ERR). Output in g_out.
bool run(const char* line) {
    shim::console_exec(line, &g_out);
    return g_out.find("ERR") == std::string::npos && g_out.size() >= 3 &&
           g_out.compare(g_out.size() - 3, 3, "OK\n") == 0;
}

bool has(const char* s) {
    return g_out.find(s) != std::string::npos;
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        console::start();
        once = true;
    }
    fake::reset_modules();
    fseq::fake::set(fseq::Status::Idle, 0);
    shim::tasks_forget();
    dmx::scene_stop();
}

}  // namespace

TEST(status_version_stats_print_and_succeed) {
    EXPECT_TRUE(run("version"));
    EXPECT_TRUE(has("version=v0.0.0-host"));
    EXPECT_TRUE(has("partition=ota_0"));
    EXPECT_TRUE(run("status"));
    EXPECT_TRUE(has("link=1"));
    EXPECT_TRUE(has("ip=192.168.2.50"));
    EXPECT_TRUE(has("dma_free="));  // status, above: the output's DMA lists come from there
    EXPECT_TRUE(run("stats"));
    EXPECT_TRUE(has("current_fps="));
    EXPECT_TRUE(has("render_decode_max_us="));
    dmx::set_cpu_load(0, 12);
    dmx::set_cpu_load(1, dmx::kCpuLoadUnknown);
    dmx::set_cpu_load(2, 50);  // no such core: ignored
    EXPECT_TRUE(run("stats"));
    EXPECT_TRUE(has("cpu0_load=12"));
    EXPECT_TRUE(has("cpu1_load=-"));
    EXPECT_FALSE(run("tasks"));  // the host build has no FreeRTOS run time stats
    EXPECT_TRUE(has("run time stats not built in"));
    EXPECT_FALSE(run("tasks now"));
    EXPECT_TRUE(run("chstat"));
}

TEST(global_get_set_and_validation) {
    EXPECT_TRUE(run("global refresh_hz 45"));
    EXPECT_EQ(config::get_global().refresh_rate_hz, 45);
    EXPECT_FALSE(run("global refresh_hz 19"));
    EXPECT_FALSE(run("global refresh_hz 121"));
    EXPECT_FALSE(run("global nope 1"));
    EXPECT_TRUE(run("global short_name stage-left"));
    EXPECT_STREQ(config::get_global().short_name, "stage-left");
    // A network setting is applied live: the box re-addresses itself.
    ::fake::reset_net();
    EXPECT_TRUE(run("global ip 10.0.0.9"));
    EXPECT_TRUE(has("note=network_applied"));
    EXPECT_EQ(::fake::net().applies, 1);
    EXPECT_EQ(::fake::net().last_ip, 0x0A000009u);
    EXPECT_TRUE(run("global dhcp 1"));
    EXPECT_EQ(::fake::net().applies, 2);
    EXPECT_TRUE(::fake::net().last_dhcp);
    EXPECT_TRUE(run("global long_name stage"));  // not a network setting
    EXPECT_EQ(::fake::net().applies, 2);
    EXPECT_FALSE(has("note=network_applied"));
    EXPECT_TRUE(run("global failsafe_color 112233"));
    EXPECT_EQ(config::get_global().failsafe_g, 0x22);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("refresh_hz=45"));
    EXPECT_TRUE(run("global refresh_hz 60"));
}

TEST(global_service_toggles_start_and_stop_them) {
    EXPECT_TRUE(run("global web_enabled 1"));
    EXPECT_TRUE(fake::modules().web_running);
    EXPECT_TRUE(run("global web_enabled 0"));
    EXPECT_FALSE(fake::modules().web_running);
    EXPECT_TRUE(run("global fpp_remote 1"));
    EXPECT_TRUE(fake::modules().fpp_running);
    EXPECT_TRUE(run("global sacn_enabled 1"));
    EXPECT_TRUE(shim::task_created("sacn_rx"));
    EXPECT_TRUE(run("global sacn_enabled 0"));
    EXPECT_TRUE(run("global fpp_remote 0"));
}

TEST(global_web_password_set_and_clear) {
    EXPECT_TRUE(run("global web_password hunter2"));
    EXPECT_TRUE(config::check_web_password("hunter2"));
    EXPECT_TRUE(run("global web_password -"));
    EXPECT_FALSE(run(("global web_password " + std::string(64, 'x')).c_str()));  // > 63
    EXPECT_FALSE(config::web_password_set());
    EXPECT_FALSE(config::web_password_set());
}

TEST(channel_get_set_and_validation) {
    EXPECT_TRUE(run("ch 2 protocol WS2812B"));
    EXPECT_TRUE(run("ch 2 pixels 300"));
    EXPECT_TRUE(run("ch 2 universe 20"));
    EXPECT_TRUE(run("ch 2 order GRB"));
    EXPECT_TRUE(run("ch 2 wb ff8000"));
    EXPECT_TRUE(run("ch 2 gaps 1:1,50:2"));
    EXPECT_TRUE(run("ch 2"));
    EXPECT_TRUE(has("protocol=WS2812B"));
    EXPECT_TRUE(has("pixels=300"));
    EXPECT_TRUE(has("gaps=1:1,50:2"));
    EXPECT_FALSE(run("ch 8"));
    EXPECT_FALSE(run("ch 2 protocol LOL"));
    EXPECT_FALSE(run("ch 2 protocol DMX512"));  // moved to the DMX node firmware
    EXPECT_FALSE(run("ch 2 protocol 9"));
    EXPECT_FALSE(run("ch 2 pixels 0"));
    EXPECT_FALSE(run("ch 2 gaps 0:1"));
    // RGBW orders: W last or first, by name or by index; W in the middle is none.
    EXPECT_TRUE(run("ch 2 order WBGR"));
    EXPECT_TRUE(run("ch 2"));
    EXPECT_TRUE(has("order=WBGR"));
    EXPECT_TRUE(run("ch 2 order 12"));
    EXPECT_TRUE(run("ch 2"));
    EXPECT_TRUE(has("order=WRGB"));
    EXPECT_FALSE(run("ch 2 order RGWB"));
    EXPECT_FALSE(run("ch 2 order 18"));
    EXPECT_TRUE(run("ch 2 order GRB"));
    EXPECT_FALSE(run("ch 2 clock_hz 100"));
    EXPECT_TRUE(run("ch 2 gaps -"));
    EXPECT_TRUE(run("ch 2 protocol Off"));
}

TEST(dmx_inject_read_and_pixel_readback) {
    EXPECT_TRUE(run("ch 0 protocol WS2815"));
    EXPECT_TRUE(run("ch 0 pixels 4"));
    EXPECT_TRUE(run("ch 0 universe 1"));
    dmx::handle_pending_remaps();
    EXPECT_TRUE(run("dmxw 1 1 aabbcc"));
    EXPECT_TRUE(has("written=3"));
    EXPECT_TRUE(run("dmxr 1 1 3"));
    EXPECT_TRUE(has("aabbcc"));
    dmx::decode_pixels_for_channel(0);
    dmx::swap_pixels(0);
    EXPECT_TRUE(run("pixr 0 0 3"));
    EXPECT_TRUE(has("aabbcc"));
    EXPECT_FALSE(run("dmxw 1 512 aabb"));  // overflows the universe
    EXPECT_FALSE(run("dmxw 999 1 aa"));    // unmapped
    EXPECT_FALSE(run("dmxw 1 1 abc"));     // odd hex
}

TEST(fx_commands_manage_the_bank) {
    std::vector<config::Effect> saved;  // the bank is shared with the cases below
    for (size_t i = 0; i < config::num_effects(); ++i)
        saved.push_back(config::get_effect(i));
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("fx1 name=Chase generator=chase color=ffffff,ff7800 speed=30 param=3 "
                    "phaser=none,0,0,0,0 invert=0 matricks=0,0,0 used=1"));
    EXPECT_TRUE(run("fx add Extra"));
    EXPECT_TRUE(has("index=8"));
    EXPECT_EQ(config::num_effects(), 9);
    EXPECT_EQ(config::get_effect(8).colors[0][1], 255);  // solid white
    EXPECT_TRUE(run("fx set 8 blobs 005aff,ff008c 40 4"));
    EXPECT_EQ(config::get_effect(8).generator, config::kSceneFxBlobs);
    EXPECT_EQ(config::effect_num_colors(config::get_effect(8)), 2);
    EXPECT_EQ(config::get_effect(8).colors[1][2], 0x8c);
    EXPECT_EQ(config::get_effect(8).speed, 40);
    EXPECT_EQ(config::get_effect(8).param, 4);
    EXPECT_TRUE(run("fx set 8 2 ffffff 0 0"));  // a generator by number
    EXPECT_EQ(config::get_effect(8).generator, config::kSceneFxRainbow);
    EXPECT_TRUE(run("fx name 8 Renamed"));
    EXPECT_STREQ(config::get_effect(8).name, "Renamed");
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("fx8 name=Renamed generator=rainbow color=ffffff speed=0 param=0 "
                    "phaser=none,0,0,0,0 invert=0 matricks=0,0,0 used=0"));

    // The dimmer phaser: what is left out keeps its value.
    EXPECT_TRUE(run("fx phaser 8 sin 20 16 128 30 reverse"));
    const auto& ph = config::get_effect(8);
    EXPECT_EQ(ph.ph_wave, config::kPhaserSin);
    EXPECT_EQ(ph.ph_rate, 20);
    EXPECT_EQ(ph.ph_spread, 16);
    EXPECT_EQ(ph.ph_width, 128);
    EXPECT_EQ(ph.ph_low, 30);
    EXPECT_EQ(ph.flags, config::kEffectPhaserReverse);
    EXPECT_TRUE(run("fx phaser 8 bump 40 8"));  // width, floor and direction kept
    EXPECT_EQ(ph.ph_wave, config::kPhaserBump);
    EXPECT_EQ(ph.ph_rate, 40);
    EXPECT_EQ(ph.ph_width, 128);
    EXPECT_EQ(ph.flags, config::kEffectPhaserReverse);
    EXPECT_TRUE(run("fx phaser 8 pwm 40 8 0 0 forward"));
    EXPECT_EQ(ph.flags, 0);
    EXPECT_TRUE(run("fx invert 8 1"));
    EXPECT_EQ(ph.flags, config::kEffectDimmerInvert);
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("phaser=pwm,40,8,0,0 invert=1 matricks=0,0,0 used=0"));
    EXPECT_TRUE(run("fx phaser 8 ramp_down 1 2 3 4 reverse"));
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("phaser=ramp_down,1,2,3,4,reverse invert=1"));
    EXPECT_TRUE(run("fx envelope 8 64 128"));  // a PWM's fade in and out
    EXPECT_EQ(ph.ph_attack, 64);
    EXPECT_EQ(ph.ph_decay, 128);
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("used=0 envelope=64,128"));
    EXPECT_FALSE(run("fx envelope 8 64"));
    EXPECT_FALSE(run("fx envelope 8 64 300"));
    EXPECT_FALSE(run("fx envelope 8 300 64"));
    EXPECT_FALSE(run("fx envelope 99 1 1"));
    EXPECT_TRUE(run("fx envelope 8 0 0"));
    EXPECT_TRUE(run("fx phaser 8 none"));  // the wave alone
    EXPECT_EQ(ph.ph_wave, config::kPhaserNone);
    EXPECT_EQ(ph.ph_rate, 1);
    EXPECT_TRUE(run("fx invert 8 0"));
    EXPECT_EQ(ph.flags, config::kEffectPhaserReverse);
    for (const char* bad :
         { "fx phaser 8", "fx phaser 8 wobble", "fx phaser 8 sin 1", "fx phaser 8 sin 1 2 3",
           "fx phaser 8 sin 256 0", "fx phaser 8 sin 1 2 3 4 sideways", "fx phaser 99 sin",
           "fx phaser 8 sin 1 2 3 4 reverse x", "fx invert 8", "fx invert 8 maybe",
           "fx invert 99 1" })
        EXPECT_FALSE(run(bad));

    // Block / Groups / Wings.
    EXPECT_TRUE(run("fx matricks 8 3 4 2"));
    EXPECT_EQ(ph.block, 3);
    EXPECT_EQ(ph.groups, 4);
    EXPECT_EQ(ph.wings, 2);
    EXPECT_TRUE(run("fx"));
    EXPECT_TRUE(has("matricks=3,4,2 used=0"));
    EXPECT_TRUE(run("fx matricks 8 0 0 0"));
    EXPECT_EQ(ph.block + ph.groups + ph.wings, 0);
    for (const char* bad : { "fx matricks 8", "fx matricks 8 1 2", "fx matricks 8 1 2 256",
                             "fx matricks 8 x 2 3", "fx matricks 99 1 2 3" })
        EXPECT_FALSE(run(bad));

    EXPECT_TRUE(run("fx move 8 0"));  // the scenes follow their effect
    EXPECT_STREQ(config::get_effect(0).name, "Renamed");
    EXPECT_EQ(config::get_scene(0).parts[0].effect, 1);
    EXPECT_FALSE(run("fx del 1"));  // "Warm white": scene 0 plays it
    EXPECT_TRUE(has("ERR"));
    EXPECT_TRUE(run("fx del 0"));
    EXPECT_EQ(config::num_effects(), 8);
    EXPECT_EQ(config::get_scene(0).parts[0].effect, 0);

    for (const char* bad :
         { "fx set 0 nope ff0000 0 0", "fx set 0 solid ff0000,00ff00,0000ff,ffffff,000000 0 0",
           "fx set 0 solid zz 0 0", "fx set 0 solid ff0000 256 0", "fx set 0 solid ff0000 0 256",
           "fx set 99 solid ff0000 0 0", "fx set 0 solid", "fx name 0", "fx name 99 x", "fx del",
           "fx del 99", "fx move 0", "fx move 0 99", "fx frobnicate" })
        EXPECT_FALSE(run(bad));
    while (config::num_effects() < config::kMaxEffects)
        EXPECT_TRUE(run("fx add"));
    EXPECT_FALSE(run("fx add"));  // full
    config::replace_effects(saved.data(), saved.size());
}

TEST(channel_fixtures_and_control_mode) {
    const auto before = config::get_channel(5);
    EXPECT_TRUE(run("ch 5 protocol WS2815"));
    EXPECT_TRUE(run("ch 5 pixels 60"));
    EXPECT_TRUE(run("ch 5 universe 30"));
    EXPECT_TRUE(run("ch 5 dmx_start 1"));
    EXPECT_TRUE(run("ch 5 fixtures 41:20:r:p1,1:20,21:20:p2"));  // any order: sorted by position
    const auto& c = config::get_channel(5);
    EXPECT_EQ(config::fixture_count(c.fixtures, config::kMaxFixtures), 3);
    EXPECT_EQ(config::fixture_profile(c.fixtures[1]), 2);
    EXPECT_TRUE(config::fixture_reversed(c.fixtures[2]));
    EXPECT_EQ(config::fixture_profile(c.fixtures[2]), 1);
    EXPECT_TRUE(run("ch 5"));
    EXPECT_TRUE(has("fixtures=1:20,21:20:p2,41:20:r:p1"));
    EXPECT_FALSE(has("patch="));  // a pixel layout has no patch sheet
    EXPECT_TRUE(has("universes=1"));

    EXPECT_TRUE(run("ch 5 packing control"));
    EXPECT_TRUE(run("ch 5"));
    EXPECT_TRUE(
        has("pixel_map=0"));  // the old layout value: fixtures alone, at the output's address
    EXPECT_TRUE(has("fixture_ctl=1"));
    EXPECT_TRUE(has("fix_universe=30"));
    EXPECT_TRUE(has("fix_dmx=1"));
    EXPECT_TRUE(has("patch=30.1+3,30.4+6,30.10+4"));
    // Both at once: the pixels at the output's address, the fixtures at theirs.
    EXPECT_TRUE(run("ch 5 pixel_map 1"));
    EXPECT_TRUE(run("ch 5 fix_universe 44"));
    EXPECT_TRUE(run("ch 5 fix_dmx 101"));
    EXPECT_TRUE(run("ch 5"));
    EXPECT_TRUE(has("pixel_map=1"));
    EXPECT_TRUE(has("packing=continuous"));
    EXPECT_TRUE(has("universes=2"));
    EXPECT_TRUE(has("patch=44.101+3,44.104+6,44.110+4"));
    EXPECT_TRUE(run("ch 5 fixture_ctl 0"));
    EXPECT_TRUE(run("ch 5"));
    EXPECT_FALSE(has("patch="));
    EXPECT_TRUE(has("universes=1"));
    EXPECT_TRUE(run("ch 5 pixel_map 0"));  // neither: the output listens to no universe
    EXPECT_TRUE(run("ch 5"));
    EXPECT_TRUE(has("universes=0"));
    EXPECT_TRUE(run("ch 5 fixture_ctl 1"));
    EXPECT_TRUE(run("ch 5 fix_universe 30"));
    EXPECT_TRUE(run("ch 5 fix_dmx 1"));
    EXPECT_TRUE(run("ch 5 fixtures -"));  // none: one fixture, the whole strip
    EXPECT_TRUE(run("ch 5"));
    EXPECT_TRUE(has("fixtures=-"));
    EXPECT_TRUE(has("patch=30.1+3"));
    for (const char* bad :
         { "ch 5 fixtures 1", "ch 5 fixtures 0:10", "ch 5 fixtures 1:0", "ch 5 fixtures 1:10:x",
           "ch 5 fixtures 1:10:p8", "ch 5 fixtures 1:10,5:10", "ch 5 fixtures 1020:10",
           "ch 5 packing sideways", "ch 5 pixel_map 2", "ch 5 fixture_ctl x",
           "ch 5 fix_universe 40000", "ch 5 fix_dmx 0", "ch 5 fix_dmx 513", "autopatch 0 control",
           "autopatch 0 fix", "autopatch 0 fix 40000" })
        EXPECT_FALSE(run(bad));
    EXPECT_TRUE(run("autopatch 0 compact whole"));  // an output keeps its switches
    EXPECT_TRUE(!config::pixel_mapped(config::get_channel(5)));
    EXPECT_TRUE(config::fixture_controlled(config::get_channel(5)));
    EXPECT_TRUE(has("fixtures "));                    // its line says where its fixtures went
    EXPECT_TRUE(run("autopatch 0 compact fix 300"));  // the fixtures' block at its own base
    EXPECT_EQ(config::fix_universe(config::get_channel(5)), 300);
    EXPECT_EQ(config::fix_dmx_start(config::get_channel(5)), 1);
    config::set_channel(5, before);
    dmx::mark_channel_dirty(5);
    dmx::handle_pending_remaps();
}

TEST(profile_commands_edit_the_bank) {
    EXPECT_TRUE(run("profile"));
    EXPECT_TRUE(has("profiles=4"));
    EXPECT_TRUE(has("profile2 name=RGB FX footprint=6 slots=red,green,blue,bank,speed,strobe"));
    EXPECT_TRUE(has("profile3 name=Full footprint=16 slots=dimmer+fine,shutter,red,green,blue,"
                    "red:1,green:1,blue:1,bank,speed,param,ph_wave,ph_rate,ph_spread,ph_width"));
    EXPECT_TRUE(run("profile add Bars"));
    EXPECT_TRUE(has("index=4"));
    EXPECT_TRUE(has("profile4 name=Bars footprint=3 slots=red,green,blue"));
    EXPECT_TRUE(run("profile slots 4 dimmer+fine,red:1,none,bank,wings"));
    const auto& p = config::get_profiles().profiles[4];
    EXPECT_EQ(p.count, 5);
    EXPECT_EQ(p.slots[0].arg, config::kProfileArgFine);
    EXPECT_EQ(p.slots[1].arg, 1);
    EXPECT_EQ(p.slots[4].fn, static_cast<uint8_t>(config::FixFn::Wings));
    EXPECT_EQ(config::profile_footprint(p), 6);
    EXPECT_TRUE(run("profile name 4 Renamed"));
    EXPECT_STREQ(p.name, "Renamed");
    EXPECT_TRUE(run("profile preset 4 dim_rgb"));
    EXPECT_EQ(config::profile_footprint(p), 4);
    EXPECT_TRUE(run("profile del 0"));  // the others move up
    EXPECT_EQ(config::get_profiles().count, 4);
    EXPECT_STREQ(config::get_profiles().profiles[0].name, "Dim RGB");
    for (const char* bad :
         { "profile slots 0", "profile slots 0 wobble", "profile slots 0 red:4",
           "profile slots 0 dimmer+coarse", "profile slots 9 red", "profile preset 0 huge",
           "profile preset 0", "profile name 0", "profile del", "profile del 9",
           "profile frobnicate 0", "profile frobnicate" })
        EXPECT_FALSE(run(bad));
    while (config::get_profiles().count < config::kMaxProfiles)
        EXPECT_TRUE(run("profile add"));
    EXPECT_FALSE(run("profile add"));             // full
    config::set_profiles(config::ProfileBank{});  // back to the presets
    EXPECT_TRUE(run("profile del 3"));
    EXPECT_TRUE(run("profile del 2"));
    EXPECT_TRUE(run("profile del 1"));
    EXPECT_FALSE(run("profile del 0"));  // one profile must stay
    config::set_profiles(config::ProfileBank{});
}

TEST(scene_commands_manage_the_list) {
    EXPECT_TRUE(run("scene"));
    EXPECT_TRUE(has("scene1 name=Chase mask=ff group=-1 parts=ff:1:each"));
    EXPECT_TRUE(run("scene add Extra"));
    EXPECT_TRUE(has("index="));
    const size_t n = config::num_scenes();
    EXPECT_EQ(config::scene_mask(config::get_scene(n - 1)), 0xFF);  // first effect, everywhere

    // Parts: outputs 5-8 take the rainbow chained; they leave the first part.
    EXPECT_TRUE(run("scene part 0 f0 2 chain"));
    EXPECT_EQ(config::get_scene(0).num_parts, 2);
    EXPECT_EQ(config::get_scene(0).parts[0].mask, 0x0F);
    EXPECT_EQ(config::get_scene(0).parts[1].mask, 0xF0);
    EXPECT_EQ(config::get_scene(0).parts[1].effect, 2);
    EXPECT_EQ(config::get_scene(0).parts[1].fixture_mode, config::kFixtureModeChain);
    EXPECT_TRUE(run("scene part 0 ff 3"));  // every output: the other parts go
    EXPECT_EQ(config::get_scene(0).num_parts, 1);
    EXPECT_EQ(config::get_scene(0).parts[0].effect, 3);
    EXPECT_EQ(config::get_scene(0).parts[0].fixture_mode, config::kFixtureModeEach);
    EXPECT_TRUE(run("scene"));
    EXPECT_TRUE(has("scene0 name=Warm white mask=ff group=-1 parts=ff:3:each"));
    // From the far end, and a default group.
    EXPECT_TRUE(run("scene part 0 ff 3 mirror rev"));
    EXPECT_TRUE(config::scene_reverse_of(config::get_scene(0).parts[0].fixture_mode));
    EXPECT_TRUE(run("scene group 0 4"));
    EXPECT_EQ(config::scene_group(config::get_scene(0)), 4);
    EXPECT_TRUE(run("scene"));
    EXPECT_TRUE(has("scene0 name=Warm white mask=ff group=4 parts=ff:3:mirror:rev"));
    EXPECT_TRUE(run("scene group 0 none"));
    EXPECT_EQ(config::scene_group(config::get_scene(0)), -1);
    EXPECT_FALSE(run("scene group 0 16"));
    EXPECT_FALSE(run("scene group 0"));
    EXPECT_TRUE(run("scene part 0 ff 3 rev"));  // the mode left out: each
    EXPECT_EQ(config::get_scene(0).parts[0].fixture_mode, config::kSceneReverseBit);
    EXPECT_TRUE(run("scene part 0 ff 3"));
    for (int o = 0; o < 8; ++o) {  // one part per output: the eight are taken
        char line[40];
        std::snprintf(line, sizeof(line), "scene part 0 %02x %d strip", 1 << o, o);
        EXPECT_TRUE(run(line));
    }
    EXPECT_EQ(config::get_scene(0).num_parts, 8);
    EXPECT_EQ(config::get_scene(0).parts[7].fixture_mode, config::kFixtureModeStrip);
    EXPECT_TRUE(run("scene clear 0"));
    EXPECT_EQ(config::get_scene(0).num_parts, 0);
    EXPECT_TRUE(run("scene part 0 ff 0"));
    for (const char* bad :
         { "scene part 0 ff", "scene part 0 00 1", "scene part 0 zz 1", "scene part 0 ff 99",
           "scene part 99 ff 0", "scene part 0 ff 0 sideways", "scene clear", "scene clear 99" })
        EXPECT_FALSE(run(bad));

    EXPECT_TRUE(run("scene play 0"));
    EXPECT_EQ(dmx::active_scene(), 0);
    {  // on a fixture group
        static config::GroupsConfig g{};
        g       = config::GroupsConfig{};
        g.count = 1;
        std::strcpy(g.groups[0].name, "A");
        g.groups[0].count      = 1;
        g.groups[0].members[0] = { 0, 0 };
        config::set_groups(g);
        EXPECT_TRUE(run("scene play 0 group 0"));
        EXPECT_TRUE(has("group=0"));
        EXPECT_FALSE(run("scene play 0 group 3"));  // no such group
        config::set_groups(config::GroupsConfig{});
    }
    EXPECT_TRUE(run("scene move 0 1"));
    EXPECT_EQ(dmx::active_scene(), 1);  // the playing scene followed
    EXPECT_TRUE(run("scene del 1"));
    EXPECT_EQ(dmx::active_scene(), -1);
    EXPECT_EQ(config::num_scenes(), n - 1);
    EXPECT_TRUE(run("scene name 0 Renamed"));
    EXPECT_STREQ(config::get_scene(0).name, "Renamed");
    EXPECT_FALSE(run("scene play 99"));
    EXPECT_TRUE(run("scene stop"));
}

TEST(global_ip_fallback) {
    EXPECT_TRUE(run("global ip_fallback artnet"));
    EXPECT_EQ(config::get_global().ip_fallback, config::kIpFallbackArtnet);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("ip_fallback=artnet"));
    EXPECT_FALSE(run("global ip_fallback 2.x"));
    EXPECT_TRUE(run("global ip_fallback linklocal"));
    EXPECT_EQ(config::get_global().ip_fallback, config::kIpFallbackLinkLocal);
}

TEST(fseq_playlist_commands_edit_and_play_it) {
    EXPECT_TRUE(run("fseq playlist clear"));
    EXPECT_TRUE(run("fseq playlist add intro.fseq 3"));
    EXPECT_TRUE(run("fseq playlist add show.fseq"));
    EXPECT_TRUE(run("fseq playlist loop on"));
    EXPECT_TRUE(run("fseq playlist autostart on"));
    const auto& p = config::get_playlist();
    EXPECT_EQ(p.count, 2);
    EXPECT_EQ(p.items[0].repeat, 3);
    EXPECT_EQ(p.loop, 1);
    EXPECT_EQ(p.autostart, 1);
    EXPECT_TRUE(run("fseq playlist"));
    EXPECT_TRUE(has("item1=show.fseq x1"));
    EXPECT_FALSE(run("fseq playlist add x.fseq 0"));  // repeat 1..255
    EXPECT_FALSE(run("fseq playlist add a/b.fseq"));  // card root only
    EXPECT_FALSE(run("fseq playlist loop maybe"));
    EXPECT_FALSE(run("fseq playlist shuffle"));
    for (int i = 0; i < 14; ++i)
        EXPECT_TRUE(run("fseq playlist add f.fseq"));
    EXPECT_FALSE(run("fseq playlist add one-too-many.fseq"));  // 16 max
    const int starts = fake::modules().playlist_starts;
    EXPECT_TRUE(run("fseq playlist play"));
    EXPECT_EQ(fake::modules().playlist_starts, starts + 1);
    EXPECT_TRUE(run("fseq playlist clear"));
    EXPECT_FALSE(run("fseq playlist play"));  // empty
    EXPECT_TRUE(run("fseq playlist autostart off"));
    EXPECT_TRUE(run("fseq play show.fseq loop"));
    EXPECT_TRUE(fake::modules().fseq_loop);
    EXPECT_FALSE(run("fseq play show.fseq twice"));
}

TEST(fseq_commands_drive_the_player) {
    EXPECT_TRUE(run("fseq list"));
    EXPECT_TRUE(has("show.fseq"));
    EXPECT_TRUE(run("fseq play show.fseq"));
    EXPECT_STREQ(fake::modules().fseq_started.c_str(), "show.fseq");
    EXPECT_FALSE(run("fseq play missing.fseq"));
    fseq::fake::set(fseq::Status::Playing, 1000);
    EXPECT_TRUE(run("fseq seek 5000"));
    EXPECT_TRUE(run("fseq stop"));
    EXPECT_TRUE(fake::modules().fseq_stopped);
}

TEST(identify_cal_loglevel_autopatch) {
    EXPECT_TRUE(run("identify 3 5"));  // 5 blinks
    EXPECT_EQ(dmx::identify_channel(), 3);
    EXPECT_FALSE(run("identify 3 0"));
    EXPECT_TRUE(run("identify all"));
    EXPECT_TRUE(run("identify all 2"));
    EXPECT_FALSE(run("identify all 99"));
    EXPECT_TRUE(run("cal 1"));
    EXPECT_EQ(fake::modules().cal_mode, 1);
    EXPECT_FALSE(run("cal 7"));
    EXPECT_TRUE(run("cal -1"));
    EXPECT_TRUE(run("loglevel warn"));
    EXPECT_FALSE(run("loglevel loud"));
    EXPECT_TRUE(run("autopatch 0"));
    EXPECT_TRUE(run("autopatch 0 compact whole"));
    EXPECT_TRUE(has("universes="));
    EXPECT_EQ(config::get_channel(0).packing, config::kPackWholePixels);
    EXPECT_FALSE(run("autopatch 0 diagonal"));
    EXPECT_TRUE(run("ch 0 packing fixture"));
    EXPECT_TRUE(run("ch 0"));
    EXPECT_TRUE(has("packing=fixture"));
    EXPECT_FALSE(run("ch 0 packing spiral"));
    EXPECT_TRUE(run("autopatch 0 continuous"));
    dmx::identify_stop();
}

TEST(overlaps_lists_the_ranges_that_share_channels) {
    config::ChannelConfig saved[config::kNumChannels];
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        saved[i]         = config::get_channel(i);
        auto c           = saved[i];
        c.protocol       = i < 2 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = 10;
        c.universe_start = 5;
        c.dmx_start      = 1;
        c.packing        = i == 1 ? config::kChanFixtureCtl : 0;
        config::set_fix_address(c, 5, 25);
        config::set_channel(i, c);
    }
    const config::ControlConfig ctl = config::get_control();
    auto on                         = ctl;
    on.enabled                      = 1;
    on.universe                     = 5;
    on.address                      = 1;
    config::set_control(on);
    EXPECT_TRUE(run("overlaps"));
    EXPECT_TRUE(has("overlaps=5"));  // each pair once, an output's own two ranges too
    EXPECT_TRUE(has("overlap=ch0 pixels,ch1 pixels,U5.1+30"));
    EXPECT_TRUE(has("overlap=ch0 pixels,control,U5.1+"));
    EXPECT_TRUE(has("overlap=ch1 pixels,ch1 fixtures,U5.25+"));
    EXPECT_FALSE(run("overlaps now"));
    for (size_t i = 0; i < config::kNumChannels; ++i)
        config::set_channel(i, saved[i]);
    config::set_control(ctl);
    EXPECT_TRUE(run("overlaps"));
}

TEST(rollback_reporting_and_ack) {
    EXPECT_TRUE(run("version"));
    EXPECT_TRUE(has("last_rollback=none"));
    EXPECT_FALSE(run("rollback ack"));  // nothing recorded
    config::RollbackRecord r{};
    std::strcpy(r.rejected_version, "v9");
    config::set_rollback(r);
    EXPECT_TRUE(run("version"));
    EXPECT_TRUE(has("acknowledged=0"));
    EXPECT_TRUE(run("rollback ack"));
    EXPECT_TRUE(run("version"));
    EXPECT_TRUE(has("acknowledged=1"));
}

TEST(reboot_and_factory_reset) {
    const int before = shim::restarts();
    run("reboot");
    EXPECT_EQ(shim::restarts(), before + 1);
    EXPECT_TRUE(run("ch 1 pixels 999"));
    fake::modules().fpp_running = true;
    fake::modules().web_running = true;
    EXPECT_TRUE(run("factory-reset"));
    EXPECT_TRUE(config::get_channel(1).pixel_count != 999);
    // The defaults turn the opt-in services off: they stop now, not at reboot.
    EXPECT_FALSE(fake::modules().fpp_running);
    EXPECT_FALSE(fake::modules().web_running);
}

TEST(global_language) {
    EXPECT_TRUE(run("global language fr"));
    EXPECT_EQ(config::get_global().language, config::kLangFrench);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("language=fr"));
    EXPECT_TRUE(run("global language 0"));
    EXPECT_EQ(config::get_global().language, config::kLangEnglish);
    EXPECT_FALSE(run("global language de"));
}

TEST(audio_test_and_tone) {
    EXPECT_TRUE(run("audio test"));
    EXPECT_TRUE(run("audio tone 440"));
    EXPECT_TRUE(run("audio tone 440 200"));
    EXPECT_FALSE(run("audio tone 5"));
    EXPECT_FALSE(run("audio tone 440 1"));
    EXPECT_FALSE(run("audio loud"));
    fake::modules().audio_ready = false;
    EXPECT_FALSE(run("audio test"));
    EXPECT_FALSE(run("audio tone 440"));
    fake::modules().audio_ready = true;
}

TEST(global_hub_preferred) {
    EXPECT_TRUE(run("global hub_preferred 1"));
    EXPECT_EQ(config::get_global().hub_preferred, 1);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("hub_preferred=1"));
    EXPECT_TRUE(run("global hub_preferred 0"));
    EXPECT_EQ(config::get_global().hub_preferred, 0);
    EXPECT_FALSE(run("global hub_preferred maybe"));
}

TEST(global_fseq_universe) {
    EXPECT_TRUE(run("global fseq_universe 17"));
    EXPECT_EQ(config::get_global().fseq_universe, 17);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("fseq_universe=17"));
    EXPECT_FALSE(run("global fseq_universe 0"));
    EXPECT_FALSE(run("global fseq_universe 40000"));
    EXPECT_TRUE(run("global fseq_universe 1"));
}

// A failed play answers ERR and a zero code: a non-zero one makes esp_console
// print its own "command returned non-zero" line after ours.
TEST(fseq_failures_keep_the_ok_err_protocol) {
    EXPECT_EQ(shim::console_exec("fseq play missing.fseq", &g_out), 0);
    EXPECT_TRUE(has("ERR"));
    config::set_playlist(config::FseqPlaylist{});
    EXPECT_EQ(shim::console_exec("fseq playlist play", &g_out), 0);
    EXPECT_TRUE(has("ERR"));
}

TEST(unknown_usage_errors_keep_the_protocol) {
    EXPECT_FALSE(run("scene frobnicate"));
    EXPECT_TRUE(has("ERR"));
    EXPECT_FALSE(run("global"
                     " a b c"));
    EXPECT_EQ(shim::console_exec("definitely-not-a-command"), -2);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}

// ── show / ctrl / zones ─────────────────────────────────────────────────────

TEST(show_sets_master_blackout_strobe_and_fade) {
    EXPECT_TRUE(run("show master 50"));
    EXPECT_EQ(dmx::master_local(0), 32767);
    EXPECT_TRUE(run("show master 100 0f"));  // outputs 1-4 back to full
    EXPECT_EQ(dmx::master_local(0), dmx::kMasterFull);
    EXPECT_EQ(dmx::master_local(7), 32767);
    EXPECT_TRUE(run("show blackout on 02"));
    EXPECT_TRUE(has("blackout=02"));
    EXPECT_TRUE(run("show blackout toggle"));
    EXPECT_EQ(dmx::blackout_local(), 0xFF);
    EXPECT_TRUE(run("show blackout off"));
    EXPECT_TRUE(run("show strobe 5"));
    EXPECT_EQ(dmx::strobe_local(3), 50);
    EXPECT_TRUE(run("show fade 1500"));
    EXPECT_EQ(config::get_global().scene_fade_ms, 1500);
    EXPECT_TRUE(run("show"));
    EXPECT_TRUE(has("master=100,100,100,100,50,50,50,50"));
    EXPECT_TRUE(has("control=off"));
    EXPECT_FALSE(run("show master 101"));
    EXPECT_FALSE(run("show strobe 26"));
    EXPECT_FALSE(run("show blackout maybe"));
    EXPECT_FALSE(run("show fade 30000"));
    EXPECT_FALSE(run("show master 50 1"));  // mask must be 2 hex digits
    run("show master 100");
    run("show strobe 0");
    run("show fade 0");
}

TEST(ctrl_composes_the_control_mode) {
    EXPECT_TRUE(run("ctrl preset full"));
    EXPECT_TRUE(has("slots=15"));
    EXPECT_TRUE(has("footprint=16"));
    EXPECT_TRUE(has("slot0 dmx=1+1 fn=master mask=ff index=0 fine=1"));
    EXPECT_FALSE(run("ctrl address 500"));  // 16 channels from 500 would pass 512
    EXPECT_TRUE(run("ctrl universe 42"));
    EXPECT_TRUE(run("ctrl enable 1"));
    EXPECT_EQ(dmx::control_universe(), 42);
    EXPECT_TRUE(run("ctrl clear"));
    EXPECT_TRUE(run("ctrl address 500"));
    EXPECT_TRUE(run("ctrl add bank 0f"));  // the functions on the effect a scene plays
    EXPECT_TRUE(run("ctrl add ph_wave"));
    EXPECT_TRUE(run("ctrl add wings"));
    EXPECT_TRUE(run("ctrl"));
    EXPECT_TRUE(has("bank"));
    EXPECT_TRUE(has("ph_wave"));
    EXPECT_TRUE(has("wings"));
    EXPECT_TRUE(run("ctrl clear"));
    EXPECT_TRUE(run("ctrl add scene 0f"));
    EXPECT_TRUE(run("ctrl add scene f0"));  // two zones from one desk
    EXPECT_TRUE(run("ctrl add red ff 2"));
    EXPECT_TRUE(run("ctrl add master ff 0 1"));
    EXPECT_TRUE(has("slot3 dmx=503+1 fn=master"));
    EXPECT_TRUE(run("ctrl set 2 blue 01 3"));
    EXPECT_TRUE(has("slot2 dmx=502 fn=blue mask=01 index=3"));
    EXPECT_TRUE(run("ctrl del 0"));
    EXPECT_TRUE(has("slots=3"));
    EXPECT_FALSE(run("ctrl add nope"));
    EXPECT_FALSE(run("ctrl address 0"));
    EXPECT_FALSE(run("ctrl set 9 master"));
    // 509..512 fits; one more 16-bit master would end past 512.
    EXPECT_TRUE(run("ctrl address 509"));
    EXPECT_FALSE(run("ctrl add master ff 0 1"));
    EXPECT_TRUE(has("past DMX channel 512"));
    EXPECT_TRUE(run("ctrl enable 0"));
    EXPECT_EQ(dmx::control_universe(), -1);
    run("ctrl address 1");
    run("ctrl preset simple");
    EXPECT_TRUE(run("ctrl clear"));
    EXPECT_TRUE(run("ctrl add direction g1"));  // on fixture group 1
    EXPECT_TRUE(has("group=1"));
    EXPECT_FALSE(run("ctrl add scene g99"));
}

TEST(scene_play_on_a_zone_and_stop_one_scene) {
    EXPECT_TRUE(run("scene play 1 0f"));
    EXPECT_TRUE(has("outputs=0f"));
    EXPECT_TRUE(run("scene play 2 f0"));
    EXPECT_TRUE(run("scene"));
    EXPECT_TRUE(has("outputs=1,1,1,1,2,2,2,2"));
    EXPECT_TRUE(run("scene stop 1"));
    EXPECT_EQ(dmx::scene_on_output(0), -1);
    EXPECT_EQ(dmx::scene_on_output(4), 2);
    EXPECT_FALSE(run("scene play 1 zz"));
    EXPECT_TRUE(run("scene stop"));
    EXPECT_EQ(dmx::active_scene(), -1);
}

// ── Every key, every usage error ────────────────────────────────────────────

TEST(global_keys_set_every_field) {
    const auto before      = config::get_global();
    const char* ok_lines[] = {
        "global dhcp 0",
        "global mask 255.255.0.0",
        "global gw 10.0.0.1",
        "global long_name A-long-box",
        "global reply_unicast 1",
        "global home_timeout_s 45",
        "global tft_brightness 40",
        "global tft_idle_dim 20",
        "global tft_dim_delay_s 90",
        "global failsafe_mode color",
        "global failsafe_scene 1",
        "global boot_scene 2",
        "global failsafe_timeout_s 12",
        "global merge_mode LTP",
        "global speaker_volume 35",
    };
    for (const char* l : ok_lines)
        EXPECT_TRUE(run(l));
    const auto& g = config::get_global();
    EXPECT_FALSE(g.use_dhcp);
    EXPECT_EQ(g.static_mask, 0xFFFF0000u);
    EXPECT_EQ(g.static_gateway, 0x0A000001u);
    EXPECT_STREQ(g.long_name, "A-long-box");
    EXPECT_TRUE(g.artnet_poll_reply_unicast);
    EXPECT_EQ(g.home_timeout_s, 45);
    EXPECT_EQ(g.tft_brightness, 40);
    EXPECT_EQ(g.tft_idle_dim, 20);
    EXPECT_EQ(g.tft_dim_delay_s, 90);
    EXPECT_EQ(g.failsafe_mode, config::kFailsafeColor);
    EXPECT_EQ(g.failsafe_scene, 1);
    EXPECT_EQ(g.boot_scene, 2);
    EXPECT_EQ(g.failsafe_timeout_s, 12);
    EXPECT_EQ(g.merge_mode, config::kMergeLtp);
    EXPECT_EQ(g.speaker_volume, 35);
    const char* bad_lines[] = {
        "global speaker_volume 101",
        "global dhcp maybe",
        "global mask 1.2.3",
        "global gw x",
        "global net 3",  // no node-wide Net/Sub-Net: unknown keys
        "global subnet 4",
        "global reply_unicast 2",
        "global home_timeout_s 70000",
        "global tft_brightness 5",
        "global tft_idle_dim 101",
        "global tft_dim_delay_s 4000",
        "global failsafe_mode sparkle",
        "global failsafe_scene 99",
        "global boot_scene 99",
        "global failsafe_timeout_s 4000",
        "global merge_mode XTP",
    };
    for (const char* l : bad_lines)
        EXPECT_FALSE(run(l));
    config::set_global(before);
}

TEST(channel_keys_set_every_field) {
    const auto before = config::get_channel(2);
    EXPECT_TRUE(run("ch 2 protocol APA102"));
    EXPECT_TRUE(run("ch 2 dmx_start 7"));
    EXPECT_TRUE(run("ch 2 brightness 80"));
    EXPECT_TRUE(run("ch 2 grouping 3"));
    EXPECT_TRUE(run("ch 2 invert 1"));
    EXPECT_TRUE(run("ch 2 gamma_x10 22"));
    EXPECT_TRUE(run("ch 2 clock_hz 2000000"));
    const auto& c = config::get_channel(2);
    EXPECT_EQ(c.dmx_start, 7);
    EXPECT_EQ(c.brightness, 80);
    EXPECT_EQ(c.grouping, 3);
    EXPECT_TRUE(c.invert_direction);
    EXPECT_EQ(c.gamma_x10, 22);
    EXPECT_EQ(c.clock_hz, 2000000u);
    for (const char* l : { "ch 2 dmx_start 0", "ch 2 brightness 256", "ch 2 grouping 9",
                           "ch 2 invert x", "ch 2 gamma_x10 9", "ch 2 sparkle 1" })
        EXPECT_FALSE(run(l));
    config::set_channel(2, before);
}

TEST(cal_loglevel_identify_and_usage_errors) {
    EXPECT_TRUE(run("cal"));
    EXPECT_TRUE(has("cal="));
    for (const char* l : { "loglevel none", "loglevel error", "loglevel info", "loglevel debug",
                           "loglevel verbose", "loglevel warn" })
        EXPECT_TRUE(run(l));
    EXPECT_TRUE(run("identify 1 2"));
    EXPECT_TRUE(run("identify stop"));
    EXPECT_EQ(dmx::identify_channel(), -1);
    const char* bad[] = { "identify",      "identify 9",       "crash",
                          "scene name 0",  "scene part 0 ff",  "scene move 0",
                          "fseq bogus",    "show blackout",    "show sparkle",
                          "ctrl universe", "ctrl preset huge", "ctrl del",
                          "ctrl sparkle" };
    for (const char* l : bad)
        EXPECT_FALSE(run(l));
}

TEST(fseq_status_and_list) {
    fseq::fake::set(fseq::Status::Playing, 1500);
    EXPECT_TRUE(run("fseq"));
    EXPECT_TRUE(has("status=playing"));
    EXPECT_TRUE(has("position_ms="));
    fseq::fake::set(fseq::Status::Error, 0);
    EXPECT_TRUE(run("fseq"));
    EXPECT_TRUE(has("error="));
    EXPECT_TRUE(run("fseq list"));
    fseq::fake::set(fseq::Status::Idle, 0);
}

TEST(show_reports_a_live_control_universe) {
    EXPECT_TRUE(run("ctrl universe 1"));
    EXPECT_TRUE(run("ctrl enable 1"));
    dmx::handle_pending_remaps();
    const uint8_t u[6] = { 0xFF, 0xFF, 0, 0, 0, 0 };
    dmx::write_universe_from_source(1, u, sizeof(u), 0x0A000001, dmx::kArtnetMergeTimeoutUs);
    EXPECT_TRUE(run("show"));
    EXPECT_TRUE(has("control=live"));
    EXPECT_TRUE(run("ctrl enable 0"));
    EXPECT_TRUE(run("ctrl universe 100"));
}
