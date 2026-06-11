// Host stub for the sACN receiver. The menu toggles it on commit of the
// ArtNet-menu "sACN" entry; the emulator has no network, so just track the
// running flag.

#include "sacn.h"

namespace pixfrog::sacn {

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

}  // namespace pixfrog::sacn
