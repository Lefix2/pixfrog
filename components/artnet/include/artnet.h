// artnet — UDP receiver for ArtNet v4 (Art-Net II legacy compatible).
//
// Listens on UDP 6454. Parses ArtDmx, ArtPoll, ArtPollReply, ArtSync.
// ArtDmx payloads are written through dmx_manager into the active "back"
// universe bank. The render task swaps banks at frame boundary.
//
// All received configuration commands are IGNORED — pixfrog's design rule
// is that configuration is local-only.

#pragma once

#include <stdint.h>

namespace pixfrog::artnet {

constexpr uint16_t kArtnetPort = 6454;

// Start the receiver task. Must be called after net interface is up and
// after dmx::init().
void start();

// Stop the receiver (rarely useful in v0; provided for tests).
void stop();

}  // namespace pixfrog::artnet
