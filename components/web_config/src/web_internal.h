// Internals shared by the web_config sources, one file per API area:
//   web_config.cpp  start/stop, route table, the SPA
//   web_util.cpp    log capture ring, OTA rollback record, JSON/body helpers,
//                   auth + cross-origin gates, protocol name tables
//   api_config.cpp  /api/config, /api/global, /api/channel, backup/restore,
//                   autopatch
//   api_show.cpp    DMX control universe + OFL fixture, /api/control,
//                   /api/show, scenes
//   api_status.cpp  /api/status, /api/diag, logs, log level, core dump
//   api_fseq.cpp    FSEQ files, play/stop, upload, playlist
//   api_system.cpp  OTA, rollback acknowledge, reboot, factory reset
//   web_push.cpp    /api/ws: status + output preview pushed over a WebSocket
//   web_mdns.cpp    unique mDNS name, elected pixfrog.local alias, /api/peers
#pragma once

#include "cJSON.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fpp_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fseq_player.h"
#include "hub_election.h"
#include "led_output.h"
#include "led_protocols.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "sacn.h"
#include "ui.h"
#include "web_config.h"
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pixfrog::web::impl {

constexpr const char* TAG = "WEB";

// ── Shared state ─────────────────────────────────────────────────────────────

extern httpd_handle_t g_server;
extern SemaphoreHandle_t g_server_mux;    // g_server vs the push task's use of it
extern std::atomic<uint32_t> g_ws_seen;   // a WebSocket client connected since start
extern std::atomic<uint32_t> g_push_run;  // web_push_task keeps going while 1

// Embedded SPA (web_ui.html baked in at link time).
extern const uint8_t web_ui_gz_start[] asm("_binary_web_ui_html_gz_start");
extern const uint8_t web_ui_gz_end[] asm("_binary_web_ui_html_gz_end");

// Log capture ring (web_util.cpp): a vprintf tee on esp_log, read by
// GET /api/logs.
constexpr size_t kLogRing = 8192;
extern char g_log_ring[kLogRing];
extern size_t g_log_head;
extern bool g_log_wrapped;
extern portMUX_TYPE g_log_mux;
extern vprintf_like_t g_prev_vprintf;
int log_vprintf(const char* fmt, va_list ap);

// OTA rollback record (web_util.cpp).
extern config::RollbackRecord g_rollback;
const char* reset_reason_str(esp_reset_reason_t r);
const config::RollbackRecord* rollback_record();
cJSON* rollback_json(const config::RollbackRecord& r);

// ── Handler helpers (web_util.cpp) ──────────────────────────────────────────

void fmt_ip(char* buf, size_t cap, uint32_t ip);
bool parse_ip(const char* s, uint32_t& out);
size_t read_body(httpd_req_t* req, char* buf, size_t max_len);
esp_err_t send_json(httpd_req_t* req, cJSON* root);
esp_err_t send_ok(httpd_req_t* req);
esp_err_t send_err(httpd_req_t* req, int code, const char* msg);
bool require_auth(httpd_req_t* req);
extern const char* const kProtoNames[];
extern const char* const kOrderNames[];
int lookup(const char* const* names, size_t count, const char* s);

// ── JSON shared between areas ────────────────────────────────────────────────

cJSON* build_control_json();  // api_show.cpp
bool apply_control_json(const cJSON* jc, config::ControlConfig& c, const char** why);
cJSON* build_show_json();
void apply_scene_json(const cJSON* js, config::Scene& sc);  // api_config.cpp
// What a global update changed that the caller must act on (POST only).
struct GlobalApplied {
    bool network = false;  // reboot to apply
    bool web_off = false;  // web_enabled went false: stop this server
    bool sacn    = false;  // sacn_enabled changed: start/stop the receiver
    bool fpp     = false;  // fpp_remote changed
};
// Shared by POST /api/global|channel and restore; see api_config.cpp.
GlobalApplied apply_global_json(const cJSON* j, config::GlobalConfig& g, const char** why);
void apply_channel_json(const cJSON* j, config::ChannelConfig& c, const char** why);
cJSON* build_playlist_json();  // api_fseq.cpp
bool apply_playlist_json(const cJSON* j, config::FseqPlaylist& p, const char** why);
cJSON* build_status_json();  // api_status.cpp

// ── Handlers (registered in web_config.cpp) ─────────────────────────────────

esp_err_t handle_root(httpd_req_t* req);
esp_err_t handle_get_config(httpd_req_t* req);
esp_err_t handle_post_global(httpd_req_t* req);
esp_err_t handle_post_channel(httpd_req_t* req);
esp_err_t handle_backup(httpd_req_t* req);
esp_err_t handle_restore(httpd_req_t* req);
esp_err_t handle_autopatch(httpd_req_t* req);
esp_err_t handle_identify(httpd_req_t* req);
esp_err_t handle_control_fixture(httpd_req_t* req);
esp_err_t handle_post_control(httpd_req_t* req);
esp_err_t handle_post_show(httpd_req_t* req);
esp_err_t handle_post_scene(httpd_req_t* req);
esp_err_t handle_scenes_add(httpd_req_t* req);
esp_err_t handle_scenes_move(httpd_req_t* req);
esp_err_t handle_scenes_stop(httpd_req_t* req);
esp_err_t handle_get_status(httpd_req_t* req);
esp_err_t handle_get_diag(httpd_req_t* req);
esp_err_t handle_get_logs(httpd_req_t* req);
esp_err_t handle_post_loglevel(httpd_req_t* req);
esp_err_t handle_coredump_get(httpd_req_t* req);
esp_err_t handle_coredump_delete(httpd_req_t* req);
esp_err_t handle_fseq_files(httpd_req_t* req);
esp_err_t handle_fseq_play(httpd_req_t* req);
esp_err_t handle_fseq_stop(httpd_req_t* req);
esp_err_t handle_fseq_upload(httpd_req_t* req);
esp_err_t handle_get_playlist(httpd_req_t* req);
esp_err_t handle_post_playlist(httpd_req_t* req);
esp_err_t handle_ota(httpd_req_t* req);
esp_err_t handle_rollback_ack(httpd_req_t* req);
esp_err_t handle_reboot(httpd_req_t* req);
esp_err_t handle_factory_reset(httpd_req_t* req);
esp_err_t handle_ws(httpd_req_t* req);
esp_err_t handle_get_peers(httpd_req_t* req);

// ── Tasks and mDNS (web_push.cpp, web_mdns.cpp) ─────────────────────────────

void web_push_task(void*);
void start_mdns();
void stop_mdns();
const char* mdns_host();
bool alias_held();
uint32_t sibling_count();

}  // namespace pixfrog::web::impl
