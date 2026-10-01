// control_console.cpp — every command through the real parser/handlers, as
// typed on the UART (shim::console_exec), against the real config_store,
// dmx_manager and sacn; hardware-facing modules are fakes.

#include <cstring>
#include <string>

#include "config_store.h"
#include "control_console.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fakes/modules_fake.h"
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
    EXPECT_TRUE(run("stats"));
    EXPECT_TRUE(has("current_fps="));
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
    EXPECT_TRUE(run("global ip 10.0.0.9"));
    EXPECT_TRUE(has("note=network_changes_apply_after_reboot"));
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

TEST(scene_commands_manage_the_list) {
    EXPECT_TRUE(run("scene"));
    EXPECT_TRUE(run("scene add Extra"));
    EXPECT_TRUE(has("index="));
    const size_t n = config::num_scenes();
    EXPECT_TRUE(run("scene set 0 blobs 005aff,ff008c 40 4 ff"));
    EXPECT_EQ(config::get_scene(0).effect, config::kSceneFxBlobs);
    EXPECT_EQ(config::scene_num_colors(config::get_scene(0)), 2);
    EXPECT_TRUE(run("scene play 0"));
    EXPECT_EQ(dmx::active_scene(), 0);
    EXPECT_TRUE(run("scene move 0 1"));
    EXPECT_EQ(dmx::active_scene(), 1);  // the playing scene followed
    EXPECT_TRUE(run("scene del 1"));
    EXPECT_EQ(dmx::active_scene(), -1);
    EXPECT_EQ(config::num_scenes(), n - 1);
    EXPECT_TRUE(run("scene name 0 Renamed"));
    EXPECT_STREQ(config::get_scene(0).name, "Renamed");
    EXPECT_FALSE(run("scene set 0 nope ff0000 0 0 ff"));
    EXPECT_FALSE(run("scene set 0 solid ff0000,00ff00,0000ff,ffffff,000000 0 0 ff"));
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
    EXPECT_TRUE(run("identify 3 5"));
    EXPECT_EQ(dmx::identify_channel(), 3);
    EXPECT_FALSE(run("identify 3 0"));
    EXPECT_TRUE(run("cal 1"));
    EXPECT_EQ(fake::modules().cal_mode, 1);
    EXPECT_FALSE(run("cal 7"));
    EXPECT_TRUE(run("cal -1"));
    EXPECT_TRUE(run("loglevel warn"));
    EXPECT_FALSE(run("loglevel loud"));
    EXPECT_TRUE(run("autopatch 0"));
    dmx::identify_stop();
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

TEST(global_hub_preferred) {
    EXPECT_TRUE(run("global hub_preferred 1"));
    EXPECT_EQ(config::get_global().hub_preferred, 1);
    EXPECT_TRUE(run("global"));
    EXPECT_TRUE(has("hub_preferred=1"));
    EXPECT_TRUE(run("global hub_preferred 0"));
    EXPECT_EQ(config::get_global().hub_preferred, 0);
    EXPECT_FALSE(run("global hub_preferred maybe"));
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
        "global net 3",
        "global subnet 4",
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
    };
    for (const char* l : ok_lines)
        EXPECT_TRUE(run(l));
    const auto& g = config::get_global();
    EXPECT_FALSE(g.use_dhcp);
    EXPECT_EQ(g.static_mask, 0xFFFF0000u);
    EXPECT_EQ(g.static_gateway, 0x0A000001u);
    EXPECT_EQ(g.artnet_net, 3);
    EXPECT_EQ(g.artnet_subnet, 4);
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
    const char* bad_lines[] = {
        "global dhcp maybe",
        "global mask 1.2.3",
        "global gw x",
        "global net 128",
        "global subnet 16",
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
    const char* bad[] = { "identify",      "identify 9",        "crash",
                          "scene name 0",  "scene set 0 solid", "scene move 0",
                          "fseq bogus",    "show blackout",     "show sparkle",
                          "ctrl universe", "ctrl preset huge",  "ctrl del",
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
