// Telemetry API: /api/status, /api/diag, logs, log level, core dump.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── GET /api/status ─────────────────────────────────────────────────────────
// Lightweight live status for SPA polling: no config blobs, just the values
// that change at runtime. Unauthenticated like the other GETs.

// mDNS identity, defined with the mDNS section below.

cJSON* build_status_json() {
    const auto st = dmx::get_stats();
    cJSON* root   = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "link", ui::is_link_up());
    cJSON_AddNumberToObject(root, "fps", static_cast<double>(st.current_fps));
    {
        const uint32_t cur_ip = ui::get_ip();
        char ipbuf[16]        = "";
        if (cur_ip) fmt_ip(ipbuf, sizeof(ipbuf), cur_ip);
        cJSON_AddStringToObject(root, "ip", ipbuf);
    }
    // Who this is on the LAN: the hub view builds its cards from these.
    cJSON_AddStringToObject(root, "name", config::get_global().short_name);
    cJSON_AddStringToObject(root, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "host", mdns_host());
    cJSON_AddBoolToObject(root, "alias", alias_held());
    cJSON_AddNumberToObject(root, "siblings", sibling_count());
    cJSON_AddNumberToObject(root, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddNumberToObject(root, "heap_min",
                            static_cast<double>(esp_get_minimum_free_heap_size()));
    cJSON_AddNumberToObject(root, "artnet_rx", static_cast<double>(st.artnet_packets_rx));
    cJSON_AddNumberToObject(root, "sacn_rx", static_cast<double>(st.sacn_packets_rx));
    cJSON_AddNumberToObject(root, "bad_rx", static_cast<double>(st.artnet_bad_packets));

    cJSON_AddNumberToObject(root, "active_scene", dmx::active_scene());
    cJSON_AddNumberToObject(root, "identify_channel", dmx::identify_channel());
    if (const auto* rb = rollback_record(); rb && !rb->acknowledged)
        cJSON_AddItemToObject(root, "rollback", rollback_json(*rb));
    // false = NVS failed at boot: changes apply but are lost at the next restart.
    cJSON_AddBoolToObject(root, "persist_ok", config::is_persistence_ok());
    cJSON_AddBoolToObject(root, "sacn_running", sacn::is_running());
    cJSON_AddBoolToObject(root, "fpp_running", fpp::is_running());

    cJSON* jf             = cJSON_CreateObject();
    const char* fseq_file = fseq::active_file();
    cJSON_AddStringToObject(jf, "active", fseq_file ? fseq_file : "");
    cJSON_AddBoolToObject(jf, "sd", fseq::sd_state() == fseq::SdState::Mounted);
    cJSON_AddNumberToObject(jf, "position_ms", fseq::position_ms());
    cJSON_AddNumberToObject(jf, "duration_ms", fseq::duration_ms());
    cJSON_AddBoolToObject(jf, "loop", fseq::looping());
    cJSON_AddNumberToObject(jf, "playlist_index", fseq::playlist_index());
    cJSON_AddItemToObject(root, "fseq", jf);

    cJSON* jchs = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        cJSON* jc = cJSON_CreateObject();
        cJSON_AddBoolToObject(jc, "active", dmx::is_channel_active(i));
        cJSON_AddBoolToObject(jc, "failsafe", dmx::is_channel_failsafe(i));
        cJSON_AddItemToArray(jchs, jc);
    }
    cJSON_AddItemToObject(root, "channels", jchs);
    cJSON_AddItemToObject(root, "show", build_show_json());
    return root;
}

esp_err_t handle_get_status(httpd_req_t* req) {
    return send_json(req, build_status_json());
}

// ── GET /api/diag — "stats for nerds" ───────────────────────────────────────
// Deeper telemetry than /api/status: render-pipeline timings, frame counters,
// memory (incl. PSRAM + frame-buffer footprint), system identity and the
// reset reason, plus per-channel capacity. Polled only while the Diagnostics
// tab is open.

esp_err_t handle_get_diag(httpd_req_t* req) {
    const auto st                  = dmx::get_stats();
    const output::DebugCounters dc = output::get_debug_counters();
    cJSON* root                    = cJSON_CreateObject();

    // Render pipeline (most recent frame).
    cJSON* jr = cJSON_CreateObject();
    cJSON_AddNumberToObject(jr, "fps", static_cast<double>(st.current_fps));
    cJSON_AddNumberToObject(jr, "encode_us", dc.encode_us);
    cJSON_AddNumberToObject(jr, "wait_us", dc.wait_us);
    cJSON_AddNumberToObject(jr, "submit_us", dc.submit_us);
    cJSON_AddNumberToObject(jr, "frame_emit_us", static_cast<double>(dmx::frame_emit_us()));
    cJSON_AddNumberToObject(jr, "frames_emitted", static_cast<double>(st.frames_emitted));
    cJSON_AddNumberToObject(jr, "trans_done", dc.trans_done);
    cJSON_AddNumberToObject(jr, "dma_underruns", st.dma_underruns);
    cJSON_AddNumberToObject(jr, "msync_err", dc.msync_err);
    cJSON_AddItemToObject(root, "render", jr);

    // Reception counters.
    cJSON* jx = cJSON_CreateObject();
    cJSON_AddNumberToObject(jx, "artnet_rx", static_cast<double>(st.artnet_packets_rx));
    cJSON_AddNumberToObject(jx, "artnet_ctrl_rx", static_cast<double>(st.artnet_ctrl_rx));
    cJSON_AddNumberToObject(jx, "sacn_rx", static_cast<double>(st.sacn_packets_rx));
    cJSON_AddNumberToObject(jx, "bad_rx", static_cast<double>(st.artnet_bad_packets));
    cJSON_AddItemToObject(root, "rx", jx);

    // Memory.
    cJSON* jm = cJSON_CreateObject();
    cJSON_AddNumberToObject(jm, "heap_free", static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddNumberToObject(jm, "heap_min", static_cast<double>(esp_get_minimum_free_heap_size()));
    cJSON_AddNumberToObject(jm, "psram_free",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    cJSON_AddNumberToObject(jm, "psram_total",
                            static_cast<double>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
    cJSON_AddNumberToObject(jm, "fb_bytes", static_cast<double>(output::fb_bytes()));
    cJSON_AddItemToObject(root, "mem", jm);

    // System identity.
    cJSON* js = cJSON_CreateObject();
    cJSON_AddNumberToObject(js, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(js, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(js, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(js, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(js, "reset_reason", reset_reason_str(esp_reset_reason()));
    if (const auto* rb = rollback_record())
        cJSON_AddItemToObject(js, "last_rollback", rollback_json(*rb));
    cJSON_AddItemToObject(root, "sys", js);

    // Per-channel capacity.
    cJSON* jchs = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        cJSON* jc = cJSON_CreateObject();
        cJSON_AddBoolToObject(jc, "active", dmx::is_channel_active(i));
        cJSON_AddBoolToObject(jc, "capacity_ok", dmx::is_channel_capacity_ok(i));
        cJSON_AddNumberToObject(jc, "max_pixels", dmx::channel_max_pixels(i));
        cJSON_AddItemToArray(jchs, jc);
    }
    cJSON_AddItemToObject(root, "channels", jchs);
    return send_json(req, root);
}

// ── GET /api/logs — recent log lines (text/plain) ───────────────────────────
// Streams the capture ring oldest-first. Sent straight from the ring without a
// snapshot: a concurrent writer can tear a byte at the seam, which is fine for
// a log view and avoids an 8 kB copy on the httpd task stack.

esp_err_t handle_get_logs(httpd_req_t* req) {
    // Logs can carry addresses and names of the show network: same gate as writes.
    if (!require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    portENTER_CRITICAL(&g_log_mux);
    const size_t head  = g_log_head;
    const bool wrapped = g_log_wrapped;
    portEXIT_CRITICAL(&g_log_mux);
    if (wrapped && kLogRing - head > 0)
        httpd_resp_send_chunk(req, g_log_ring + head, kLogRing - head);
    if (head > 0) httpd_resp_send_chunk(req, g_log_ring, head);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ── POST /api/loglevel — {"level":"none|error|warn|info|debug|verbose"} ──────

esp_err_t handle_post_loglevel(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[64];
    const size_t n = read_body(req, buf, sizeof(buf) - 1);
    if (n == 0) return send_err(req, 400, "empty body");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "bad json");
    const cJSON* it     = cJSON_GetObjectItemCaseSensitive(j, "level");
    esp_log_level_t lvl = ESP_LOG_INFO;
    bool ok             = cJSON_IsString(it);
    if (ok) {
        const char* s = it->valuestring;
        if (strcasecmp(s, "none") == 0)
            lvl = ESP_LOG_NONE;
        else if (strcasecmp(s, "error") == 0)
            lvl = ESP_LOG_ERROR;
        else if (strcasecmp(s, "warn") == 0)
            lvl = ESP_LOG_WARN;
        else if (strcasecmp(s, "info") == 0)
            lvl = ESP_LOG_INFO;
        else if (strcasecmp(s, "debug") == 0)
            lvl = ESP_LOG_DEBUG;
        else if (strcasecmp(s, "verbose") == 0)
            lvl = ESP_LOG_VERBOSE;
        else
            ok = false;
    }
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, "level: none|error|warn|info|debug|verbose");
    esp_log_level_set("*", lvl);
    return send_ok(req);
}

// ── GET/DELETE /api/coredump ────────────────────────────────────────────────
// Raw core-dump image from the coredump partition (24-byte esp_core_dump
// header + ELF) — exactly the input `espcoredump.py info_corefile --core
// <file> --core-format raw build/pixfrog.elf` expects. 404 when no crash
// is stored (or the partition is absent — pre-coredump tables in the field).

esp_err_t handle_coredump_get(httpd_req_t* req) {
    // Raw RAM: may hold a cleartext password from an earlier Basic-auth request.
    if (!require_auth(req)) return ESP_OK;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"no coredump\"}");
    }
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!part) return send_err(req, 500, "no coredump partition");

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pixfrog-coredump.bin\"");
    static char buf[4096];  // httpd stack is small; one download at a time
    size_t off       = addr - part->address;
    size_t remaining = size;
    while (remaining > 0) {
        const size_t n = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            httpd_resp_sendstr_chunk(req, nullptr);
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
        off       += n;
        remaining -= n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t handle_coredump_delete(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (esp_core_dump_image_erase() != ESP_OK) return send_err(req, 500, "erase failed");
    return send_ok(req);
}

}  // namespace pixfrog::web::impl
