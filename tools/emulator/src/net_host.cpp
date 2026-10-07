// Host stub for the net component: the emulator has no network, the menu's
// address edits are stored and nothing re-addresses.

#include "net.h"

namespace pixfrog::net {

void init(esp_netif_t*, Publisher) {}

void apply(const config::GlobalConfig&) {}

bool link_up() {
    return true;
}

}  // namespace pixfrog::net
