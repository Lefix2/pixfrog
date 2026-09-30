// pixfrog_api_host — the real web_config handlers (with the real config_store,
// dmx_manager and sacn behind them) served on a local TCP port, for browser
// tests of the SPA against the actual API instead of a hand-written mock.
//
//   pixfrog_api_host [--port 8080] [--rollback] [--password <pwd>]
//
// State starts from factory defaults (in-memory NVS) on every launch.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config_store.h"
#include "dmx_manager.h"
#include "shim_control.h"
#include "web_config.h"

namespace {
volatile bool g_stop = false;
void on_signal(int) {
    g_stop = true;
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t port        = 8080;
    bool rollback        = false;
    const char* password = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rollback"))
            rollback = true;
        else if (!std::strcmp(argv[i], "--password") && i + 1 < argc)
            password = argv[++i];
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
    pixfrog::web::start();
    std::printf("pixfrog_api_host listening on http://127.0.0.1:%u\n", port);
    std::fflush(stdout);
    shim::http_serve(port, &g_stop);
    return 0;
}
