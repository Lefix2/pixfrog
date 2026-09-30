// Test-side control of the IDF shims: fake clock, in-memory NVS with fault
// injection, log verbosity. Only the harness tests include this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shim {

// ── Clock (esp_timer_get_time, FreeRTOS ticks) ──────────────────────────────
void set_time_us(int64_t t);
void advance_us(int64_t dt);
void advance_ms(int64_t dt);
int64_t now_us();

// ── NVS ─────────────────────────────────────────────────────────────────────
void nvs_wipe();  // empty flash, faults cleared
bool nvs_has(const std::string& ns, const std::string& key);
std::vector<uint8_t> nvs_raw(const std::string& ns, const std::string& key);
void nvs_put_raw(const std::string& ns, const std::string& key, const void* data, size_t len);
void nvs_erase_key(const std::string& ns, const std::string& key);
// Fault injection: the next N calls of that kind fail (0 = never).
void nvs_fail_init(int count);   // nvs_flash_init returns an error
void nvs_fail_erase(int count);  // nvs_flash_erase returns an error
void nvs_fail_open(int count);   // nvs_open returns an error
int nvs_writes();                // nvs_set_blob calls since the last wipe

// ── Tasks ───────────────────────────────────────────────────────────────────
// xTaskCreate* records the task instead of spawning it; the test runs its
// body on the calling thread (it must return — see net_on_idle).
bool run_task(const char* name);
bool task_created(const char* name);
void tasks_forget();

// ── Network fabric (lwip/sockets.h) ─────────────────────────────────────────
struct Datagram {
    uint32_t ip;  // host order
    uint16_t port;
    std::vector<uint8_t> bytes;
};
// Queue a datagram for the socket bound to `port`, from `from_ip` (host order).
void net_push(uint16_t port, const std::vector<uint8_t>& bytes, uint32_t from_ip = 0xC0A80201);
// Called when a bound socket's queue is empty (the receive would block):
// typically the component's stop(), so its task loop returns to the test.
void net_on_idle(uint16_t port, void (*fn)());
std::vector<Datagram>& net_sent();                // everything sendto() emitted, oldest first
std::vector<uint32_t> net_groups(uint16_t port);  // joined multicast groups (host order)
void net_reset();

// ── Console / system ────────────────────────────────────────────────────────
// Runs one console line through the registered command table as the REPL
// would; returns the command's exit code and fills `out` with its stdout.
int console_exec(const char* line, std::string* out = nullptr);
int restarts();   // esp_restart() calls
int log_level();  // last esp_log_level_set("*", …)

}  // namespace shim
