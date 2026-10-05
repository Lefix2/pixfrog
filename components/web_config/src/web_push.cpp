// /api/ws: status and the output preview pushed over a WebSocket.

#include "web_internal.h"

namespace pixfrog::web::impl {

std::atomic<uint32_t> g_ws_seen{ 0 };  // 32-bit: P4 RMW rule
std::atomic<uint32_t> g_push_run{ 0 };
SemaphoreHandle_t g_server_mux = nullptr;
constexpr size_t kMaxWsClients = 8;

// Frames handed to the httpd task and not sent yet. A client that stopped
// reading holds that task in send() until the socket times out, frame after
// frame. Unbounded, the six frames a second queued behind it ate the whole
// PSRAM and no HTTP request got through any more (bench, 2026-10-05: 14 kB
// of PSRAM left after 7 h, the web server silent, ping fine). Two in
// flight are plenty: a frame the server has no time for is dropped — the next
// one is fresher anyway.
constexpr uint32_t kMaxPending = 2;
std::atomic<uint32_t> g_ws_pending{ 0 };  // 32-bit: P4 RMW rule

esp_err_t handle_ws(httpd_req_t* req) {
    if (req->method == HTTP_GET) {  // the handshake: httpd answered it already
        g_ws_seen.store(1, std::memory_order_relaxed);
        return ESP_OK;
    }
    // The client sends nothing meaningful: read a frame and drop it.
    httpd_ws_frame_t f{};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len == 0) return ESP_OK;
    if (f.len > 128) return ESP_FAIL;  // not ours to buffer
    uint8_t buf[128];
    f.payload = buf;
    return httpd_ws_recv_frame(req, &f, f.len);
}

// A frame for every WebSocket client; `data` comes from frame_alloc() and is
// freed by ws_broadcast. Frames live in PSRAM: under 16 KB a plain malloc
// lands in internal RAM, and 5 frames a second fragmented it until the
// display's DMA allocations failed (screen freeze, then glitches).
struct WsPush {
    httpd_handle_t server;
    uint8_t* data;
    size_t len;
    httpd_ws_type_t type;
};

static uint8_t* frame_alloc(size_t len) {
    void* p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return static_cast<uint8_t*>(p ? p : malloc(len));
}

// Runs in the httpd task: send the frame to every WebSocket client. With none
// left, the push task goes back to sleep until the next handshake.
static void ws_broadcast(void* arg) {
    auto* p  = static_cast<WsPush*>(arg);
    size_t n = kMaxWsClients;
    int fds[kMaxWsClients];
    size_t sent = 0;
    if (httpd_get_client_list(p->server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; ++i) {
            if (httpd_ws_get_fd_info(p->server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            ++sent;
            httpd_ws_frame_t f{};
            f.type    = p->type;
            f.payload = p->data;
            f.len     = p->len;
            // A send that failed timed out on a client that reads no more:
            // close it, or every later frame waits out the timeout again. A
            // browser that is still there reconnects.
            if (httpd_ws_send_frame_async(p->server, fds[i], &f) != ESP_OK)
                httpd_sess_trigger_close(p->server, fds[i]);
        }
    }
    if (!sent) g_ws_seen.store(0, std::memory_order_relaxed);
    free(p->data);
    delete p;
    g_ws_pending.fetch_sub(1, std::memory_order_acq_rel);
}

// The httpd task still holds the frames it was given: no room for another.
static bool ws_backlog_full() {
    return g_ws_pending.load(std::memory_order_acquire) >= kMaxPending;
}

// Hand a frame to the httpd task; takes ownership of `data`. Dropped when the
// task is behind (ws_backlog_full) or the server is down.
static void ws_queue(uint8_t* data, size_t len, httpd_ws_type_t type) {
    auto* p = ws_backlog_full() ? nullptr : new (std::nothrow) WsPush{ nullptr, data, len, type };
    bool queued = false;
    xSemaphoreTake(g_server_mux, portMAX_DELAY);
    if (p && g_server) {
        p->server = g_server;
        g_ws_pending.fetch_add(1, std::memory_order_acq_rel);
        queued = httpd_queue_work(g_server, ws_broadcast, p) == ESP_OK;
        if (!queued) g_ws_pending.fetch_sub(1, std::memory_order_acq_rel);
    }
    xSemaphoreGive(g_server_mux);
    if (queued) return;
    free(data);
    delete p;
}

// Live output preview, a binary frame: 'P', the output count, then per output
// its sample count n and n RGB triplets (dmx::output_preview; n = 0 when Off).
constexpr size_t kPreviewSamples = 64;
constexpr size_t kPreviewBytes   = 2 + config::kNumChannels * (1 + kPreviewSamples * 3);

static void push_preview() {
    auto* buf = frame_alloc(kPreviewBytes);
    if (!buf) return;
    size_t len = 0;
    buf[len++] = 'P';
    buf[len++] = static_cast<uint8_t>(config::kNumChannels);
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const size_t n  = dmx::output_preview(ch, buf + len + 1, kPreviewSamples);
        buf[len]        = static_cast<uint8_t>(n);
        len            += 1 + n * 3;
    }
    ws_queue(buf, len, HTTPD_WS_TYPE_BINARY);
}

static void push_status() {
    cJSON* root = build_status_json();
    cJSON_AddStringToObject(root, "type", "status");
    constexpr size_t kMax = 4096;  // the status is ~1 KB
    auto* text            = frame_alloc(kMax);
    const bool ok         = text && cJSON_PrintPreallocated(root, reinterpret_cast<char*>(text),
                                                            static_cast<int>(kMax), false);
    cJSON_Delete(root);
    if (ok)
        ws_queue(text, std::strlen(reinterpret_cast<char*>(text)), HTTPD_WS_TYPE_TEXT);
    else
        free(text);
}

// 5 Hz preview, 1 Hz status, while a WebSocket client is connected.
void web_push_task(void*) {
    uint32_t tick = 0;
    while (g_push_run.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!g_ws_seen.load(std::memory_order_relaxed)) continue;  // nobody listening yet
        if (ws_backlog_full()) continue;  // the server is behind: build nothing it would drop
        push_preview();
        if (++tick % 5 == 0) push_status();
    }
    vTaskDelete(nullptr);
}

}  // namespace pixfrog::web::impl
