#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::sacn {

// Start the sACN receiver task (UDP 5568 + multicast joins for every
// configured universe, refreshed periodically). No-op if already running.
void start();

// Stop the receiver and leave all multicast groups. No-op if not running.
void stop();

bool is_running();

// The Ethernet MAC passes only the multicast addresses in its filter table (8
// on the ESP32-P4 EMAC, some taken by mDNS and all-hosts), and sACN needs one
// per universe: past that, the MAC dropped the frames of the extra universes
// even though lwIP had joined them (bench: universes 9+ of 16 never arrived).
// The hook is called with true once more than kHwMulticastSlots groups are
// joined — the board then lets the MAC pass all multicast and lwIP keeps only
// the joined groups — and with false when back under it, and on stop. Set it
// once, before start().
constexpr size_t kHwMulticastSlots = 5;
void set_multicast_overflow_hook(void (*hook)(bool pass_all));

}  // namespace pixfrog::sacn
