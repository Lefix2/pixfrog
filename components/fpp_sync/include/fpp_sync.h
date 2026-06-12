// FPP MultiSync remote — slaves local FSEQ playback to an FPP/xSchedule
// master on the LAN. Opt-in (GlobalConfig::fpp_remote): no socket is open
// when disabled.

#pragma once

namespace pixfrog::fpp {

// Open the UDP socket (port 32320, multicast 239.70.80.80) and spawn the
// receiver task. Idempotent.
void start();

// Close the socket and stop the task. Does not stop a running playback.
void stop();

bool is_running();

}  // namespace pixfrog::fpp
