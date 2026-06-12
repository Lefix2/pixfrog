// Host stub for the FPP MultiSync receiver. The menu toggles it on commit
// of the ArtNet-menu "FPP" entry; the emulator has no network, so just
// track the running flag.

#include "fpp_sync.h"

namespace pixfrog::fpp {

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

}  // namespace pixfrog::fpp
