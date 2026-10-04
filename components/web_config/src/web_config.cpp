#include "web_config.h"

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── GET / → SPA ─────────────────────────────────────────────────────────────

// Content hash of the embedded page (FNV-1a), computed once: the ETag must
// change whenever the page does, including dev builds that keep the version.
// The two linker symbols bound one blob, but to the compiler they are unrelated
// objects: comparing pointers across them is undefined and GCC turned a
// `p < end` walk into an infinite loop. Take the length through integers.
static size_t web_ui_gz_len() {
    return reinterpret_cast<uintptr_t>(web_ui_gz_end) -
           reinterpret_cast<uintptr_t>(web_ui_gz_start);
}

static const char* web_ui_etag() {
    static char etag[12] = {};
    if (!etag[0]) {
        uint32_t h       = 2166136261u;
        const size_t len = web_ui_gz_len();
        for (size_t i = 0; i < len; ++i)
            h = (h ^ web_ui_gz_start[i]) * 16777619u;
        snprintf(etag, sizeof(etag), "\"%08lx\"", static_cast<unsigned long>(h));
    }
    return etag;
}

// Revalidated on every load ("no-cache"): a 304 costs a few bytes, and a new
// firmware's page is picked up at once.
esp_err_t handle_root(httpd_req_t* req) {
    const char* etag = web_ui_etag();
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char inm[16];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(web_ui_gz_start), web_ui_gz_len());
}

}  // namespace pixfrog::web::impl

namespace pixfrog::web {

using namespace impl;

// ── Public API ───────────────────────────────────────────────────────────────

void init_log_capture() {
    if (g_prev_vprintf) return;  // already installed
    g_prev_vprintf = esp_log_set_vprintf(&log_vprintf);
    ESP_LOGI(TAG, "log capture ring installed (%u B)", static_cast<unsigned>(kLogRing));
}

// One route, every other httpd_uri_t field zero (the WebSocket build adds
// some), so the table below stays clean under -Wmissing-field-initializers.
static httpd_uri_t route(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*),
                         bool websocket = false) {
    httpd_uri_t r{};
    r.uri          = uri;
    r.method       = method;
    r.handler      = handler;
    r.is_websocket = websocket;
    return r;
}

void start() {
    if (g_server) return;
    if (!g_server_mux) g_server_mux = xSemaphoreCreateMutex();
    if (!g_server_mux) return;

    init_log_capture();  // ensure capture is on even if app_main didn't call it

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 40;  // 35 routes today
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 8192;  // esp_ota_* calls need headroom over the 4 kB default

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        g_server = nullptr;
        return;
    }

    const httpd_uri_t routes[] = {
        route("/", HTTP_GET, handle_root),
        route("/api/config", HTTP_GET, handle_get_config),
        route("/api/global", HTTP_POST, handle_post_global),
        route("/api/channel/*", HTTP_POST, handle_post_channel),
        route("/api/identify", HTTP_POST, handle_identify),
        route("/api/audio/test", HTTP_POST, handle_audio_test),
        route("/api/ota", HTTP_POST, handle_ota),
        route("/api/backup", HTTP_GET, handle_backup),
        route("/api/restore", HTTP_POST, handle_restore),
        route("/api/scene/*", HTTP_POST, handle_post_scene),
        route("/api/scenes/add", HTTP_POST, handle_scenes_add),
        route("/api/scenes/move", HTTP_POST, handle_scenes_move),
        route("/api/rollback/ack", HTTP_POST, handle_rollback_ack),
        route("/api/show", HTTP_POST, handle_post_show),
        route("/api/control", HTTP_POST, handle_post_control),
        route("/api/control/fixture", HTTP_GET, handle_control_fixture),
        route("/api/scenes/stop", HTTP_POST, handle_scenes_stop),
        route("/api/reboot", HTTP_POST, handle_reboot),
        route("/api/factory-reset", HTTP_POST, handle_factory_reset),
        route("/api/autopatch", HTTP_POST, handle_autopatch),
        route("/api/fseq/files", HTTP_GET, handle_fseq_files),
        route("/api/fseq/playlist", HTTP_GET, handle_get_playlist),
        route("/api/fseq/playlist", HTTP_POST, handle_post_playlist),
        route("/api/groups", HTTP_POST, handle_post_groups),
        route("/api/fseq/play", HTTP_POST, handle_fseq_play),
        route("/api/fseq/stop", HTTP_POST, handle_fseq_stop),
        route("/api/fseq/upload", HTTP_POST, handle_fseq_upload),
        route("/api/status", HTTP_GET, handle_get_status),
        route("/api/ws", HTTP_GET, handle_ws, true),
        route("/api/peers", HTTP_GET, handle_get_peers),
        route("/api/coredump", HTTP_GET, handle_coredump_get),
        route("/api/coredump", HTTP_DELETE, handle_coredump_delete),
        route("/api/diag", HTTP_GET, handle_get_diag),
        route("/api/logs", HTTP_GET, handle_get_logs),
        route("/api/loglevel", HTTP_POST, handle_post_loglevel),
    };
    for (const auto& r : routes)
        httpd_register_uri_handler(g_server, &r);

    ESP_LOGI(TAG, "HTTP server started on port %u", cfg.server_port);
    start_mdns();

    g_ws_seen.store(0, std::memory_order_relaxed);
    if (!g_push_run.exchange(1, std::memory_order_acq_rel) &&
        xTaskCreate(web_push_task, "web_push", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_push_run.store(0, std::memory_order_release);
        ESP_LOGW(TAG, "web_push task create failed: the SPA keeps polling");
    }
}

void stop() {
    if (!g_server) return;
    g_push_run.store(0, std::memory_order_release);  // the push task leaves at its next tick
    xSemaphoreTake(g_server_mux, portMAX_DELAY);
    httpd_stop(g_server);
    g_server = nullptr;
    xSemaphoreGive(g_server_mux);
    stop_mdns();
    ESP_LOGI(TAG, "HTTP server stopped");
}

bool is_running() {
    return g_server != nullptr;
}

}  // namespace pixfrog::web
