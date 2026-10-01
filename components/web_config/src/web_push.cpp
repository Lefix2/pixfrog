// /api/ws: status and the output preview pushed over a WebSocket.

#include "web_internal.h"

namespace pixfrog::web::impl {

std::atomic<uint32_t> g_ws_seen{ 0 };  // 32-bit: P4 RMW rule
std::atomic<uint32_t> g_push_run{ 0 };
SemaphoreHandle_t g_server_mux = nullptr;
constexpr size_t kMaxWsClients = 8;

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

// A frame for every WebSocket client; `data` is malloc'ed (cJSON's allocator
// for text) and freed by ws_broadcast.
struct WsPush {
    httpd_handle_t server;
    uint8_t* data;
    size_t len;
    httpd_ws_type_t type;
};

// Runs in the httpd task: send the frame to every WebSocket client.
static void ws_broadcast(void* arg) {
    auto* p  = static_cast<WsPush*>(arg);
    size_t n = kMaxWsClients;
    int fds[kMaxWsClients];
    if (httpd_get_client_list(p->server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; ++i) {
            if (httpd_ws_get_fd_info(p->server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            httpd_ws_frame_t f{};
            f.type    = p->type;
            f.payload = p->data;
            f.len     = p->len;
            httpd_ws_send_frame_async(p->server, fds[i], &f);
        }
    }
    if (p->type == HTTPD_WS_TYPE_TEXT)
        cJSON_free(p->data);
    else
        free(p->data);
    delete p;
}

// Hand a frame to the httpd task; takes ownership of `data`.
static void ws_queue(uint8_t* data, size_t len, httpd_ws_type_t type) {
    auto* p     = new (std::nothrow) WsPush{ nullptr, data, len, type };
    bool queued = false;
    xSemaphoreTake(g_server_mux, portMAX_DELAY);
    if (p && g_server) {
        p->server = g_server;
        queued    = httpd_queue_work(g_server, ws_broadcast, p) == ESP_OK;
    }
    xSemaphoreGive(g_server_mux);
    if (queued) return;
    if (type == HTTPD_WS_TYPE_TEXT)
        cJSON_free(data);
    else
        free(data);
    delete p;
}

// Live output preview, a binary frame: 'P', the output count, then per output
// its sample count n and n RGB triplets (dmx::output_preview; n = 0 when Off).
constexpr size_t kPreviewSamples = 64;
constexpr size_t kPreviewBytes   = 2 + config::kNumChannels * (1 + kPreviewSamples * 3);

static void push_preview() {
    auto* buf = static_cast<uint8_t*>(malloc(kPreviewBytes));
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
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) ws_queue(reinterpret_cast<uint8_t*>(text), std::strlen(text), HTTPD_WS_TYPE_TEXT);
}

// 5 Hz preview, 1 Hz status, once a client has connected.
void web_push_task(void*) {
    uint32_t tick = 0;
    while (g_push_run.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!g_ws_seen.load(std::memory_order_relaxed)) continue;  // nobody listening yet
        push_preview();
        if (++tick % 5 == 0) push_status();
    }
    vTaskDelete(nullptr);
}

}  // namespace pixfrog::web::impl
