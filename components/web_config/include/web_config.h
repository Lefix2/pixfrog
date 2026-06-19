#pragma once

namespace pixfrog::web {

// Begin capturing esp_log into the ring exposed by GET /api/logs (tees to UART
// too). Idempotent. Call early in app_main so boot logs are captured even when
// the web server stays disabled; start() also calls it.
void init_log_capture();

// Start the HTTP server (port 80). No-op if already running.
void start();

// Stop the HTTP server and close the listening socket. No-op if not running.
void stop();

bool is_running();

}  // namespace pixfrog::web
