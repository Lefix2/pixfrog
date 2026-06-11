// Host stub for web_config. The menu toggles the web server on commit of the
// "Web UI" network entry; the emulator has no HTTP server, so just track the
// running flag.

#include "web_config.h"

namespace pixfrog::web {

namespace {
bool g_running = false;
}

void start() {
    g_running = true;
}

void stop() {
    g_running = false;
}

bool is_running() {
    return g_running;
}

}  // namespace pixfrog::web
