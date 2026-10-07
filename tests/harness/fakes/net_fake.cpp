#include "net_fake.h"

#include "net.h"

namespace fake {
Net& net() {
    static Net n;
    return n;
}
void reset_net() {
    net() = Net{};
}
}  // namespace fake

namespace pixfrog {
namespace net {
void init(esp_netif_t*, Publisher) {}
void apply(const config::GlobalConfig& g) {
    auto& n = ::fake::net();
    ++n.applies;
    n.last_dhcp = g.use_dhcp;
    n.last_ip   = g.static_ip;
    n.last_mask = g.static_mask;
    n.last_gw   = g.static_gateway;
}
bool link_up() {
    return ::fake::net().link_up;
}
}  // namespace net
}  // namespace pixfrog
