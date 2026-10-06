// Stand-in for the net component in the suites that do not boot the box:
// records what net::apply() was asked for.
#pragma once
#include <cstdint>
namespace fake {
struct Net {
    int applies      = 0;     // net::apply() calls
    bool last_dhcp   = true;  // the addressing of the last call
    uint32_t last_ip = 0, last_mask = 0, last_gw = 0;
    bool link_up = true;  // net::link_up()
};
Net& net();
void reset_net();
}  // namespace fake
