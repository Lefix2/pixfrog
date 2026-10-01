// Host implementations of the IDF APIs the portable firmware components use.
// Single-threaded by design: tests drive every task explicitly, and a blocking
// call advances the fake clock by its timeout instead of sleeping.

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "shim_control.h"

// ── Clock ────────────────────────────────────────────────────────────────────

namespace {
int64_t g_now_us  = 1'000'000;  // not 0: firmware treats 0 as "never happened"
bool g_real_clock = false;
int64_t mono_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t g_real_base = 0;
}  // namespace

namespace shim {
void set_time_us(int64_t t) {
    g_now_us = t;
}
void advance_us(int64_t dt) {
    g_now_us += dt;
}
void advance_ms(int64_t dt) {
    g_now_us += dt * 1000;
}
int64_t now_us() {
    return g_real_clock ? 1'000'000 + mono_us() - g_real_base : g_now_us;
}
void use_real_clock(bool on) {
    g_real_clock = on;
    g_real_base  = mono_us();
}
bool real_clock() {
    return g_real_clock;
}
}  // namespace shim

int64_t esp_timer_get_time() {
    return shim::now_us();
}

const char* esp_err_to_name(esp_err_t code) {
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    default: return "ESP_ERR";
    }
}

// ── Heap ─────────────────────────────────────────────────────────────────────

void* heap_caps_calloc(size_t n, size_t size, uint32_t) {
    if (shim::should_fail(shim::Fault::HeapCaps)) return nullptr;
    return std::calloc(n, size);
}
void* heap_caps_malloc(size_t size, uint32_t) {
    if (shim::should_fail(shim::Fault::HeapCaps)) return nullptr;
    return std::malloc(size);
}
void heap_caps_free(void* p) {
    std::free(p);
}
size_t heap_caps_get_free_size(uint32_t) {
    return 24u << 20;
}
namespace {
bool g_psram          = true;
size_t g_psram_blocks = 0;
}  // namespace
namespace shim {
void psram_present(bool present) {
    g_psram = present;
}
void psram_largest_block(size_t bytes) {
    g_psram_blocks = bytes;
}
}  // namespace shim
size_t heap_caps_get_largest_free_block(uint32_t) {
    return g_psram_blocks ? g_psram_blocks : 16u << 20;
}
size_t heap_caps_get_total_size(uint32_t) {
    return g_psram ? 32u << 20 : 0;
}
void* heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t) {
    if (shim::should_fail(shim::Fault::HeapCaps)) return nullptr;
    void* p         = nullptr;
    const size_t sz = (n * size + alignment - 1) / alignment * alignment;
    if (posix_memalign(&p, alignment, sz) != 0) return nullptr;
    std::memset(p, 0, sz);
    return p;
}
size_t heap_caps_get_minimum_free_size(uint32_t) {
    return 23u << 20;
}

// ── Random (deterministic) ───────────────────────────────────────────────────

namespace {
uint32_t g_rng = 0x2545F491u;
}
uint32_t esp_random() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
void esp_fill_random(void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i)
        p[i] = static_cast<uint8_t>(esp_random());
}

// ── NVS ──────────────────────────────────────────────────────────────────────

namespace {
std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_nvs;
std::map<nvs_handle_t, std::string> g_handles;
nvs_handle_t g_next_handle = 1;
int g_fail_init = 0, g_fail_erase = 0, g_fail_open = 0, g_writes = 0;

bool consume(int& budget) {
    if (budget <= 0) return false;
    --budget;
    return true;
}
}  // namespace

namespace shim {
void nvs_wipe() {
    g_nvs.clear();
    g_handles.clear();
    g_fail_init = g_fail_erase = g_fail_open = g_writes = 0;
}
bool nvs_has(const std::string& ns, const std::string& key) {
    auto n = g_nvs.find(ns);
    return n != g_nvs.end() && n->second.count(key);
}
std::vector<uint8_t> nvs_raw(const std::string& ns, const std::string& key) {
    return nvs_has(ns, key) ? g_nvs[ns][key] : std::vector<uint8_t>{};
}
void nvs_put_raw(const std::string& ns, const std::string& key, const void* data, size_t len) {
    const auto* p  = static_cast<const uint8_t*>(data);
    g_nvs[ns][key] = std::vector<uint8_t>(p, p + len);
}
void nvs_erase_key(const std::string& ns, const std::string& key) {
    if (g_nvs.count(ns)) g_nvs[ns].erase(key);
}
void nvs_fail_init(int count) {
    g_fail_init = count;
}
void nvs_fail_erase(int count) {
    g_fail_erase = count;
}
void nvs_fail_open(int count) {
    g_fail_open = count;
}
int nvs_writes() {
    return g_writes;
}
}  // namespace shim

esp_err_t nvs_flash_init() {
    return consume(g_fail_init) ? ESP_ERR_NVS_NO_FREE_PAGES : ESP_OK;
}
esp_err_t nvs_flash_erase() {
    if (consume(g_fail_erase)) return ESP_FAIL;
    g_nvs.clear();
    return ESP_OK;
}
esp_err_t nvs_open(const char* ns, nvs_open_mode_t, nvs_handle_t* out) {
    if (consume(g_fail_open)) return ESP_FAIL;
    *out            = g_next_handle++;
    g_handles[*out] = ns;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) {
    g_handles.erase(h);
}
esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
    auto hs = g_handles.find(h);
    if (hs == g_handles.end()) return ESP_ERR_INVALID_ARG;
    if (!shim::nvs_has(hs->second, key)) return ESP_ERR_NVS_NOT_FOUND;
    const auto& blob = g_nvs[hs->second][key];
    if (!out) {
        *len = blob.size();
        return ESP_OK;
    }
    if (*len < blob.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(out, blob.data(), blob.size());
    *len = blob.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* data, size_t len) {
    auto hs = g_handles.find(h);
    if (hs == g_handles.end()) return ESP_ERR_INVALID_ARG;
    shim::nvs_put_raw(hs->second, key, data, len);
    ++g_writes;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    auto hs = g_handles.find(h);
    if (hs == g_handles.end()) return ESP_ERR_INVALID_ARG;
    shim::nvs_erase_key(hs->second, key);
    return ESP_OK;
}

// ── FreeRTOS ─────────────────────────────────────────────────────────────────

struct shim_sem {
    bool mutex;
    int count;
};
struct shim_eg {
    EventBits_t bits;
};

SemaphoreHandle_t xSemaphoreCreateMutex() {
    if (shim::should_fail(shim::Fault::Semaphore)) return nullptr;
    return new shim_sem{ true, 1 };
}
SemaphoreHandle_t xSemaphoreCreateBinary() {
    if (shim::should_fail(shim::Fault::Semaphore)) return nullptr;
    return new shim_sem{ false, 0 };
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    if (s->count > 0) {
        --s->count;
        return pdTRUE;
    }
    // Nobody else can give it on a single thread: the wait times out.
    if (ticks != portMAX_DELAY) shim::advance_ms(ticks);
    return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (s->count == 1) return pdFALSE;  // binary / mutex saturate at 1
    s->count = 1;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t* woken) {
    const BaseType_t r = xSemaphoreGive(s);
    if (woken && r == pdTRUE) *woken = pdTRUE;
    return r;
}
void vSemaphoreDelete(SemaphoreHandle_t s) {
    delete s;
}
EventGroupHandle_t xEventGroupCreate() {
    if (shim::should_fail(shim::Fault::EventGroup)) return nullptr;
    return new shim_eg{ 0 };
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits) {
    g->bits |= bits;
    return g->bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits) {
    const EventBits_t before  = g->bits;
    g->bits                  &= ~bits;
    return before;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t g) {
    return g->bits;
}
namespace shim {
void task_delay_point();  // net.cpp: run_task_for's budget
}
void vTaskDelay(TickType_t ticks) {
    if (shim::real_clock())
        std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
    else
        shim::advance_ms(ticks);
    shim::task_delay_point();
}
void vTaskDelayUntil(TickType_t* previous_wake, TickType_t increment) {
    *previous_wake    += increment;
    const auto now_ms  = static_cast<TickType_t>(shim::now_us() / 1000);
    if (*previous_wake > now_ms) shim::advance_ms(*previous_wake - now_ms);
    shim::task_delay_point();
}
TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(shim::now_us() / 1000);
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                       UBaseType_t prio, TaskHandle_t* out) {
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}
void vTaskDelete(TaskHandle_t) {}

// ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────────────

namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
void block(mbedtls_sha256_context* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
               (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4],
             f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                            kK[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                            ((a & b) ^ (a & cc) ^ (b & cc));
        h  = g;
        g  = f;
        f  = e;
        e  = d + t1;
        d  = cc;
        cc = b;
        b  = a;
        a  = t1 + t2;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}
}  // namespace

void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
    std::memset(ctx, 0, sizeof(*ctx));
}
void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
    std::memset(ctx, 0, sizeof(*ctx));
}
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::memcpy(ctx->state, iv, sizeof(iv));
    ctx->bits = 0;
    ctx->used = 0;
    return 0;
}
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* in, size_t len) {
    ctx->bits += static_cast<uint64_t>(len) * 8;
    while (len--) {
        ctx->buf[ctx->used++] = *in++;
        if (ctx->used == 64) {
            block(ctx, ctx->buf);
            ctx->used = 0;
        }
    }
    return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char out[32]) {
    const uint64_t bits = ctx->bits;
    const uint8_t pad   = 0x80;
    mbedtls_sha256_update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while (ctx->used != 56)
        mbedtls_sha256_update(ctx, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    mbedtls_sha256_update(ctx, len, 8);
    for (int i = 0; i < 8; ++i) {
        out[4 * i]     = static_cast<uint8_t>(ctx->state[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(ctx->state[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(ctx->state[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(ctx->state[i]);
    }
    return 0;
}
