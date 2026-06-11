#pragma once

#include <stdint.h>

namespace pixfrog::sacn {

// Start the sACN receiver task (UDP 5568 + multicast joins for every
// configured universe, refreshed periodically). No-op if already running.
void start();

// Stop the receiver and leave all multicast groups. No-op if not running.
void stop();

bool is_running();

}  // namespace pixfrog::sacn
