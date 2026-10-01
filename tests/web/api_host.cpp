// pixfrog_api_host — the real web_config handlers (with the real config_store,
// dmx_manager and sacn behind them) served on a local TCP port, for browser
// tests of the SPA against the actual API instead of a hand-written mock.
//
//   pixfrog_api_host [--port 8080] [--rollback] [--password <pwd>] [--demo]
//
// --demo seeds a show-sized setup and keeps it looking alive (traffic, FPS,
// active channels, a scene and a sequence playing) for the documentation
// screenshots (tools/screenshots/).
//
// State starts from factory defaults (in-memory NVS) on every launch.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "config_store.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "fseq_player.h"
#include "shim_control.h"
#include "web_config.h"

namespace {
volatile bool g_stop = false;

using pixfrog::led::Protocol;

// A front-of-house rig: eight lines of mixed fixtures, the factory scenes,
// the full control preset on universe 100, a three-item playlist.
void seed_demo() {
    namespace cfg = pixfrog::config;
    struct Line {
        Protocol p;
        uint16_t px;
    };
    const Line lines[cfg::kNumChannels] = {
        { Protocol::WS2815, 300 }, { Protocol::WS2815, 300 }, { Protocol::WS2812B, 144 },
        { Protocol::SK6812, 120 }, { Protocol::APA102, 240 }, { Protocol::WS2811, 50 },
        { Protocol::WS2815, 512 }, { Protocol::Off, 0 },
    };
    for (size_t ch = 0; ch < cfg::kNumChannels; ++ch) {
        auto c        = cfg::get_channel(ch);
        c.protocol    = lines[ch].p;
        c.pixel_count = lines[ch].px ? lines[ch].px : c.pixel_count;
        cfg::set_channel(ch, c);
        pixfrog::dmx::mark_channel_dirty(ch);
    }
    auto g = cfg::get_global();
    std::strcpy(g.short_name, "pixfrog-stage");
    std::strcpy(g.long_name, "pixfrog · stage left");
    g.sacn_enabled  = true;
    g.scene_fade_ms = 1500;
    cfg::set_global(g);
    auto ctl     = cfg::default_control();
    ctl.enabled  = 1;
    ctl.universe = 100;
    cfg::control_apply_preset(ctl, cfg::ControlPreset::Full);
    cfg::set_control(ctl);
    cfg::FseqPlaylist pl{};
    const char* names[]  = { "intro.fseq", "show.fseq", "loop.fseq" };
    const uint8_t reps[] = { 1, 2, 1 };
    for (size_t i = 0; i < 3; ++i) {
        std::strcpy(pl.items[i].name, names[i]);
        pl.items[i].repeat = reps[i];
    }
    pl.count = 3;
    pl.loop  = 1;
    cfg::set_playlist(pl);
    pixfrog::dmx::mark_global_dirty();
    pixfrog::dmx::handle_pending_remaps();
    pixfrog::dmx::scene_start_on(2, 0x30, 0);  // Rainbow on outputs 5-6
    pixfrog::fseq::start("show.fseq", true);
    pixfrog::fseq::fake::set(pixfrog::fseq::Status::Playing, 42'000);
}

// Art-Net traffic, 60 FPS and per-line activity, as a live rig shows them.
void keep_demo_alive() {
    for (;;) {
        for (size_t ch = 0; ch < 7; ++ch)
            if (ch != 4 && ch != 5) pixfrog::dmx::note_channel_activity(ch);
        for (int i = 0; i < 25; ++i)
            pixfrog::dmx::note_packet_rx();
        pixfrog::dmx::set_current_fps(60);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void on_signal(int) {
    g_stop = true;
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t port        = 8080;
    bool rollback        = false;
    const char* password = nullptr;
    bool demo            = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rollback"))
            rollback = true;
        else if (!std::strcmp(argv[i], "--password") && i + 1 < argc)
            password = argv[++i];
        else if (!std::strcmp(argv[i], "--demo"))
            demo = true;
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    shim::use_real_clock(true);
    shim::nvs_wipe();
    pixfrog::config::init();
    pixfrog::dmx::init();
    // One lit WS2815 line so the dashboard and editors have something to show.
    auto c        = pixfrog::config::get_channel(0);
    c.protocol    = pixfrog::led::Protocol::WS2815;
    c.pixel_count = 60;
    pixfrog::config::set_channel(0, c);
    if (rollback) {
        pixfrog::config::RollbackRecord r{};
        std::strcpy(r.rejected_version, "v9.9.9-bad");
        std::strcpy(r.rejected_slot, "ota_1");
        std::strcpy(r.running_version, "v0.0.0-host");
        r.reset_reason = 6;  // task-wdt
        pixfrog::config::set_rollback(r);
    }
    if (password) pixfrog::config::set_web_password(password);
    if (demo) {
        seed_demo();
        std::thread(keep_demo_alive).detach();
    }
    pixfrog::web::start();
    std::printf("pixfrog_api_host listening on http://127.0.0.1:%u\n", port);
    std::fflush(stdout);
    shim::http_serve(port, &g_stop);
    return 0;
}
