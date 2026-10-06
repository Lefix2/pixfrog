// pixfrog — the box's IP addressing: static or DHCP, applied at boot and
// again, live, whenever a setting changes (web, console, device menu,
// ArtIpProg). Knows the Ethernet netif and the link; tells the rest of the
// box what it has through one publisher, so it depends on nothing above it.
#pragma once
#include <cstdint>

#include "config_store.h"

// The netif, as esp_netif_types.h declares it: callers that only apply a
// setting need no IDF header.
struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;

namespace pixfrog {
namespace net {

enum class State : uint8_t {
    Disconnected,  // no link: a configured address is not an address yet
    Acquiring,     // link up, waiting for a DHCP lease
    Connected,     // an address usable on the wire
};

// Called with the address usable on the wire (host order, 0 for none) every
// time it or the link changes.
using Publisher = void (*)(uint32_t host_order_ip, State state);

// Takes the netif once the Ethernet driver is attached to it: registers the
// link and IP handlers and applies the stored addressing ahead of link-up.
void init(esp_netif_t* netif, Publisher publish);

// Applies the addressing of `g` now: a static address, or a fresh DHCP lease
// (the Art-Net fallback again if no server answers). The box keeps running;
// only the address changes. A no-op before init().
void apply(const config::GlobalConfig& g);

bool link_up();

}  // namespace net
}  // namespace pixfrog
