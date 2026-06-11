#pragma once

namespace pixfrog::web {

// Start the HTTP server (port 80). No-op if already running.
void start();

// Stop the HTTP server and close the listening socket. No-op if not running.
void stop();

bool is_running();

}  // namespace pixfrog::web
