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
    EXPECT_TRUE(run("factory-reset"));
    EXPECT_TRUE(config::get_channel(1).pixel_count != 999);
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
