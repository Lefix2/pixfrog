// Test-side control of the IDF shims: fake clock, in-memory NVS with fault
// injection, log verbosity. Only the harness tests include this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace shim {

// ── Clock (esp_timer_get_time, FreeRTOS ticks) ──────────────────────────────
void set_time_us(int64_t t);
void advance_us(int64_t dt);
void advance_ms(int64_t dt);
int64_t now_us();
// Serve mode (browser tests): time follows the wall clock, blocking calls sleep.
void use_real_clock(bool on);
bool real_clock();

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
// Runs a task that never returns (a monitor loop) until it has blocked
// `max_delays` times (vTaskDelay / vTaskDelayUntil), then unwinds back here.
// `on_delay`, when set, runs at every one of those blocking points.
bool run_task_for(const char* name, int max_delays, void (*on_delay)() = nullptr);
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

// ── HTTP (esp_http_server) ──────────────────────────────────────────────────
struct HttpResponse {
    int status = 0;  // parsed from the status line, 200 when unset
    std::string status_line;
    std::string content_type;
    std::map<std::string, std::string> headers;
    std::string body;
    bool handled = false;  // a registered handler matched
};
HttpResponse http_request(const char* method, const std::string& uri,
                          const std::string& body                           = std::string(),
                          const std::map<std::string, std::string>& headers = {});
// Largest chunk one httpd_req_recv() returns (0 = whole body): exercises the
// handlers' body-reassembly loops.
void http_recv_chunk(size_t max);
bool http_running();
size_t http_routes();  // handlers registered (esp_http_server caps it)
// Serves the registered handlers on a real TCP port until *stop is set
// (browser tests); requests are dispatched on the calling thread.
void http_serve(uint16_t port, volatile bool* stop);

// ── Fault injection ─────────────────────────────────────────────────────────
// The next `count` calls of that API fail (error paths of the code under test).
enum class Fault {
    Socket,       // socket() → -1
    Bind,         // bind() → -1
    Join,         // setsockopt(IP_ADD_MEMBERSHIP) → -1
    SendTo,       // sendto() → -1
    HeapCaps,     // heap_caps_calloc/malloc → nullptr
    Semaphore,    // xSemaphoreCreate* → nullptr
    EventGroup,   // xEventGroupCreate → nullptr
    OtaNoTarget,  // esp_ota_get_next_update_partition → nullptr
    OtaBegin,     // esp_ota_begin → ESP_FAIL
    OtaWrite,     // esp_ota_write → ESP_FAIL
    OtaSetBoot,   // esp_ota_set_boot_partition → ESP_FAIL
    HttpdStart,   // httpd_start → ESP_FAIL
    TaskCreate,   // xTaskCreate* → pdFAIL
    MdnsInit,     // mdns_init → ESP_FAIL
    Count,
};
// `skip` calls succeed first (fail the Nth allocation, not the first).
void fail_next(Fault f, int count = 1, int skip = 0);
bool should_fail(Fault f);  // consumes one pending failure
void faults_clear();
void set_reset_reason(int reason);  // esp_reset_reason() value

// ── SD card (driver/sdmmc_host.h, esp_vfs_fat.h, vfs_redirect.h) ────────────
void sd_root(const std::string& host_dir);  // files of the card
void sd_insert(bool inserted);              // mount succeeds / status OK only when in

// ── OTA / core dump / mDNS ──────────────────────────────────────────────────
std::vector<uint8_t>& ota_image();                     // bytes esp_ota_write() received
bool ota_boot_switched();                              // esp_ota_set_boot_partition() called
void ota_fail_validation(bool fail);                   // esp_ota_end() rejects the image
void coredump_set(const std::vector<uint8_t>& image);  // empty = none
void mdns_add_peer(const char* instance, uint32_t ip, const char* product, const char* node,
                   const char* fw);
void mdns_reset();

}  // namespace shim
