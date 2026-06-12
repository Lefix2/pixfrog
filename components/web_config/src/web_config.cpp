#include "web_config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mdns.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_protocols.h"
#include "sacn.h"
#include "ui.h"

namespace pixfrog::web {

namespace {

constexpr const char* TAG = "WEB";

httpd_handle_t g_server = nullptr;

// Embedded SPA (web_ui.html baked in at link time).
extern const uint8_t web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[] asm("_binary_web_ui_html_end");

// ── JSON helpers ─────────────────────────────────────────────────────────────

static void fmt_ip(char* buf, size_t cap, uint32_t ip) {
    snprintf(buf, cap, "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
             static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
             static_cast<unsigned>(ip & 0xFFu));
}

static bool parse_ip(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

// Read request body (up to max_len bytes). Returns actual length, or 0 on error.
static size_t read_body(httpd_req_t* req, char* buf, size_t max_len) {
    int remaining = req->content_len;
    if (remaining <= 0 || static_cast<size_t>(remaining) > max_len) return 0;
    size_t off = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + off, static_cast<size_t>(remaining));
        if (ret <= 0) return 0;
        off       += ret;
        remaining -= ret;
    }
    buf[off] = '\0';
    return off;
}

static esp_err_t send_json(httpd_req_t* req, cJSON* root) {
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return r;
}

static esp_err_t send_ok(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_err(httpd_req_t* req, int code, const char* msg) {
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, buf);
}

// ── HTTP Basic auth gate (mutating endpoints only) ──────────────────────────
// No password configured (the default) = open. Otherwise every POST must
// carry `Authorization: Basic base64(user:password)`; the user part is
// ignored. 401 + WWW-Authenticate makes browsers show their native prompt
// and re-send credentials for the realm. A flat 500 ms delay on every
// failure keeps LAN brute force impractical without lockout bookkeeping.

static bool authorized(httpd_req_t* req) {
    if (!config::web_password_set()) return true;

    char header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        return false;
    if (strncmp(header, "Basic ", 6) != 0) return false;

    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              reinterpret_cast<const unsigned char*>(header + 6),
                              strlen(header + 6)) != 0)
        return false;
    decoded[decoded_len] = '\0';

    // user:password — the user part is free-form and ignored.
    const char* colon = strchr(reinterpret_cast<const char*>(decoded), ':');
    if (!colon) return false;
    return config::check_web_password(colon + 1);
}

// Returns true when the request may proceed; otherwise the 401 has been sent.
static bool require_auth(httpd_req_t* req) {
    if (authorized(req)) return true;
    vTaskDelay(pdMS_TO_TICKS(500));  // flat anti-brute-force cost
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"pixfrog\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return false;
}

// ── Protocol / color-order name tables ──────────────────────────────────────

static const char* const kProtoNames[] = { "Off",    "WS2815", "WS2812B", "WS2811",  "SK6812",
                                           "WS2814", "APA102", "SK9822",  "LPD8806", "DMX512" };
static_assert(sizeof(kProtoNames) / sizeof(kProtoNames[0]) ==
              static_cast<size_t>(led::Protocol::COUNT));

static const char* const kOrderNames[] = { "RGB", "RBG",  "GRB",  "GBR",  "BRG",
                                           "BGR", "RGBW", "GRBW", "RGBWW" };
static_assert(sizeof(kOrderNames) / sizeof(kOrderNames[0]) ==
              static_cast<size_t>(led::ColorOrder::COUNT));

static int lookup(const char* const* names, size_t count, const char* s) {
    for (size_t i = 0; i < count; ++i)
        if (strcasecmp(names[i], s) == 0) return static_cast<int>(i);
    return -1;
}

// ── GET / → SPA ─────────────────────────────────────────────────────────────

static esp_err_t handle_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const size_t len = web_ui_html_end - web_ui_html_start;
    return httpd_resp_send(req, reinterpret_cast<const char*>(web_ui_html_start), len);
}

// ── GET /api/config ─────────────────────────────────────────────────────────

static cJSON* build_global_json() {
    const auto& g = config::get_global();
    cJSON* jg     = cJSON_CreateObject();
    cJSON_AddBoolToObject(jg, "dhcp", g.use_dhcp);

    char ip[16], mask[16], gw[16];
    fmt_ip(ip, sizeof(ip), g.static_ip);
    fmt_ip(mask, sizeof(mask), g.static_mask);
    fmt_ip(gw, sizeof(gw), g.static_gateway);
    cJSON_AddStringToObject(jg, "ip", ip);
    cJSON_AddStringToObject(jg, "mask", mask);
    cJSON_AddStringToObject(jg, "gw", gw);
    cJSON_AddNumberToObject(jg, "net", g.artnet_net);
    cJSON_AddNumberToObject(jg, "subnet", g.artnet_subnet);
    cJSON_AddStringToObject(jg, "short_name", g.short_name);
    cJSON_AddStringToObject(jg, "long_name", g.long_name);
    cJSON_AddBoolToObject(jg, "reply_unicast", g.artnet_poll_reply_unicast);
    cJSON_AddNumberToObject(jg, "refresh_hz", g.refresh_rate_hz);
    cJSON_AddNumberToObject(jg, "home_timeout_s", g.home_timeout_s);
    cJSON_AddBoolToObject(jg, "web_enabled", g.web_enabled);
    cJSON_AddBoolToObject(jg, "sacn_enabled", g.sacn_enabled);
    cJSON_AddBoolToObject(jg, "fpp_remote", g.fpp_remote);
    cJSON_AddBoolToObject(jg, "auth_enabled", config::web_password_set());
    cJSON_AddNumberToObject(jg, "failsafe_mode", g.failsafe_mode);
    cJSON_AddNumberToObject(jg, "failsafe_timeout_s", g.failsafe_timeout_s);
    cJSON_AddNumberToObject(jg, "failsafe_scene", g.failsafe_scene);
    cJSON_AddNumberToObject(jg, "boot_scene", g.boot_scene);
    cJSON_AddNumberToObject(jg, "merge_mode", g.merge_mode);
    char fscol[8];
    snprintf(fscol, sizeof(fscol), "#%02x%02x%02x", g.failsafe_r, g.failsafe_g, g.failsafe_b);
    cJSON_AddStringToObject(jg, "failsafe_color", fscol);
    return jg;
}

static cJSON* build_scenes_json() {
    cJSON* jscenes = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumScenes; ++i) {
        const auto& sc = config::get_scene(i);
        cJSON* js      = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", sc.name);
        cJSON_AddNumberToObject(js, "effect", sc.effect);
        char col[8];
        snprintf(col, sizeof(col), "#%02x%02x%02x", sc.r, sc.g, sc.b);
        cJSON_AddStringToObject(js, "color", col);
        cJSON_AddNumberToObject(js, "speed", sc.speed);
        cJSON_AddNumberToObject(js, "param", sc.param);
        cJSON_AddNumberToObject(js, "mask", sc.channel_mask);
        cJSON_AddItemToArray(jscenes, js);
    }
    return jscenes;
}

static cJSON* build_channels_json() {
    cJSON* jchs = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        const auto& c = config::get_channel(i);
        cJSON* jc     = cJSON_CreateObject();
        cJSON_AddNumberToObject(jc, "id", static_cast<double>(i));
        cJSON_AddStringToObject(jc, "protocol", kProtoNames[static_cast<size_t>(c.protocol)]);
        cJSON_AddStringToObject(jc, "color_order", kOrderNames[static_cast<size_t>(c.color_order)]);
        cJSON_AddNumberToObject(jc, "universe_start", c.universe_start);
        cJSON_AddNumberToObject(jc, "dmx_start", c.dmx_start);
        cJSON_AddNumberToObject(jc, "pixel_count", c.pixel_count);
        cJSON_AddNumberToObject(jc, "brightness", c.brightness);
        cJSON_AddNumberToObject(jc, "grouping", c.grouping);
        cJSON_AddBoolToObject(jc, "invert", c.invert_direction);
        cJSON_AddNumberToObject(jc, "clock_hz", static_cast<double>(c.clock_hz));
        cJSON_AddNumberToObject(jc, "gamma_x10", c.gamma_x10);
        char wb[8];
        snprintf(wb, sizeof(wb), "#%02x%02x%02x", c.wb_r, c.wb_g, c.wb_b);
        cJSON_AddStringToObject(jc, "wb", wb);
        cJSON_AddItemToArray(jchs, jc);
    }
    return jchs;
}

// ── GET /api/config ─────────────────────────────────────────────────────────

static esp_err_t handle_get_config(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "link", ui::is_link_up());
    cJSON_AddNumberToObject(root, "fps", static_cast<double>(dmx::get_stats().current_fps));
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "ota_partition", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(root, "active_scene", dmx::active_scene());
    cJSON_AddNumberToObject(root, "identify_channel", dmx::identify_channel());
    const char* fseq_file = fseq::active_file();
    cJSON_AddStringToObject(root, "active_fseq", fseq_file ? fseq_file : "");
    cJSON_AddItemToObject(root, "global", build_global_json());
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    return send_json(req, root);
}

// ── GET /api/status ─────────────────────────────────────────────────────────
// Lightweight live status for SPA polling: no config blobs, just the values
// that change at runtime. Unauthenticated like the other GETs.

static esp_err_t handle_get_status(httpd_req_t* req) {
    const auto st = dmx::get_stats();
    cJSON* root   = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "link", ui::is_link_up());
    cJSON_AddNumberToObject(root, "fps", static_cast<double>(st.current_fps));
    cJSON_AddNumberToObject(root, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddNumberToObject(root, "heap_min",
                            static_cast<double>(esp_get_minimum_free_heap_size()));
    cJSON_AddNumberToObject(root, "artnet_rx", static_cast<double>(st.artnet_packets_rx));
    cJSON_AddNumberToObject(root, "sacn_rx", static_cast<double>(st.sacn_packets_rx));
    cJSON_AddNumberToObject(root, "bad_rx", static_cast<double>(st.artnet_bad_packets));

    cJSON_AddNumberToObject(root, "active_scene", dmx::active_scene());
    cJSON_AddNumberToObject(root, "identify_channel", dmx::identify_channel());
    cJSON_AddBoolToObject(root, "sacn_running", sacn::is_running());
    cJSON_AddBoolToObject(root, "fpp_running", fpp::is_running());

    cJSON* jf             = cJSON_CreateObject();
    const char* fseq_file = fseq::active_file();
    cJSON_AddStringToObject(jf, "active", fseq_file ? fseq_file : "");
    cJSON_AddBoolToObject(jf, "sd", fseq::sd_state() == fseq::SdState::Mounted);
    cJSON_AddNumberToObject(jf, "position_ms", fseq::position_ms());
    cJSON_AddNumberToObject(jf, "duration_ms", fseq::duration_ms());
    cJSON_AddItemToObject(root, "fseq", jf);

    cJSON* jchs = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        cJSON* jc = cJSON_CreateObject();
        cJSON_AddBoolToObject(jc, "active", dmx::is_channel_active(i));
        cJSON_AddBoolToObject(jc, "failsafe", dmx::is_channel_failsafe(i));
        cJSON_AddItemToArray(jchs, jc);
    }
    cJSON_AddItemToObject(root, "channels", jchs);
    return send_json(req, root);
}

// ── GET/DELETE /api/coredump ────────────────────────────────────────────────
// Raw ELF post-mortem from the coredump partition. 404 when no crash is
// stored (or the partition is absent — pre-coredump tables in the field).

static esp_err_t handle_coredump_get(httpd_req_t* req) {
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
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pixfrog-coredump.elf\"");
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

static esp_err_t handle_coredump_delete(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (esp_core_dump_image_erase() != ESP_OK) return send_err(req, 500, "erase failed");
    return send_ok(req);
}

// ── GET /api/backup + POST /api/restore ─────────────────────────────────────
// Backup = the persisted configuration only (no live status, no password
// hash). Restore applies best-effort: unknown or invalid fields are skipped
// so a backup from a different firmware degrades gracefully.

static esp_err_t handle_backup(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "backup_version", 1);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddItemToObject(root, "global", build_global_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pixfrog-config.json\"");
    return send_json(req, root);
}

static void restore_global(cJSON* jg) {
    config::GlobalConfig g = config::get_global();
    cJSON* it;
    auto num = [&](const char* k, double lo, double hi, double* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        if (cJSON_IsNumber(it) && it->valuedouble >= lo && it->valuedouble <= hi) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    auto getb = [&](const char* k, bool* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        if (cJSON_IsBool(it)) {
            *out = cJSON_IsTrue(it);
            return true;
        }
        return false;
    };
    auto getip = [&](const char* k, uint32_t* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        uint32_t x;
        if (cJSON_IsString(it) && parse_ip(it->valuestring, x)) *out = x;
    };
    double v;
    bool bv;
    if (getb("dhcp", &bv)) g.use_dhcp = bv;
    getip("ip", &g.static_ip);
    getip("mask", &g.static_mask);
    getip("gw", &g.static_gateway);
    if (num("net", 0, 127, &v)) g.artnet_net = static_cast<uint8_t>(v);
    if (num("subnet", 0, 15, &v)) g.artnet_subnet = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jg, "short_name");
    if (cJSON_IsString(it)) {
        memset(g.short_name, 0, sizeof(g.short_name));
        strncpy(g.short_name, it->valuestring, sizeof(g.short_name) - 1);
    }
    it = cJSON_GetObjectItemCaseSensitive(jg, "long_name");
    if (cJSON_IsString(it)) {
        memset(g.long_name, 0, sizeof(g.long_name));
        strncpy(g.long_name, it->valuestring, sizeof(g.long_name) - 1);
    }
    if (getb("reply_unicast", &bv)) g.artnet_poll_reply_unicast = bv;
    if (num("refresh_hz", 30, 60, &v) && (v == 30 || v == 60))
        g.refresh_rate_hz = static_cast<uint8_t>(v);
    if (num("home_timeout_s", 0, 65535, &v)) g.home_timeout_s = static_cast<uint16_t>(v);
    if (getb("web_enabled", &bv)) g.web_enabled = bv;
    if (getb("sacn_enabled", &bv)) g.sacn_enabled = bv;
    if (num("failsafe_mode", 0, 3, &v)) g.failsafe_mode = static_cast<uint8_t>(v);
    if (num("failsafe_timeout_s", 0, 3600, &v)) g.failsafe_timeout_s = static_cast<uint16_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jg, "failsafe_color");
    if (cJSON_IsString(it)) {
        unsigned cr, cg, cb;
        if (sscanf(it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring,
                   "%02x%02x%02x", &cr, &cg, &cb) == 3) {
            g.failsafe_r = static_cast<uint8_t>(cr);
            g.failsafe_g = static_cast<uint8_t>(cg);
            g.failsafe_b = static_cast<uint8_t>(cb);
        }
    }
    if (num("failsafe_scene", 0, config::kNumScenes - 1, &v))
        g.failsafe_scene = static_cast<uint8_t>(v);
    if (num("boot_scene", 0, config::kNumScenes, &v)) g.boot_scene = static_cast<uint8_t>(v);
    if (num("merge_mode", 0, 1, &v)) g.merge_mode = static_cast<uint8_t>(v);
    config::set_global(g);
}

static void restore_channel(size_t i, cJSON* jc) {
    auto c    = config::get_channel(i);
    cJSON* it = cJSON_GetObjectItemCaseSensitive(jc, "protocol");
    if (cJSON_IsString(it)) {
        const int pv = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT),
                              it->valuestring);
        if (pv >= 0) c.protocol = static_cast<led::Protocol>(pv);
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "color_order");
    if (cJSON_IsString(it)) {
        const int ov = lookup(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT),
                              it->valuestring);
        if (ov >= 0) c.color_order = static_cast<led::ColorOrder>(ov);
    }
    auto num = [&](const char* k, double lo, double hi, double* out) {
        it = cJSON_GetObjectItemCaseSensitive(jc, k);
        if (cJSON_IsNumber(it) && it->valuedouble >= lo && it->valuedouble <= hi) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    double v;
    if (num("universe_start", 1, 32767, &v)) c.universe_start = static_cast<uint16_t>(v);
    if (num("dmx_start", 1, 512, &v)) c.dmx_start = static_cast<uint16_t>(v);
    if (num("pixel_count", 1, dmx::kMaxPixelsPerChan, &v)) c.pixel_count = static_cast<uint16_t>(v);
    if (num("brightness", 0, 255, &v)) c.brightness = static_cast<uint8_t>(v);
    if (num("grouping", 1, 8, &v)) c.grouping = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jc, "invert");
    if (cJSON_IsBool(it)) c.invert_direction = cJSON_IsTrue(it);
    if (num("clock_hz", 100'000, led::kPclkHz / 2, &v)) c.clock_hz = static_cast<uint32_t>(v);
    if (num("gamma_x10", 10, 40, &v)) c.gamma_x10 = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jc, "wb");
    if (cJSON_IsString(it)) {
        unsigned cr, cg, cb;
        if (sscanf(it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring,
                   "%02x%02x%02x", &cr, &cg, &cb) == 3) {
            c.wb_r = cr ? static_cast<uint8_t>(cr) : 255;
            c.wb_g = cg ? static_cast<uint8_t>(cg) : 255;
            c.wb_b = cb ? static_cast<uint8_t>(cb) : 255;
        }
    }
    config::set_channel(i, c);
    dmx::mark_channel_dirty(i);
}

static void restore_scene(size_t i, cJSON* js) {
    auto sc   = config::get_scene(i);
    cJSON* it = cJSON_GetObjectItemCaseSensitive(js, "name");
    if (cJSON_IsString(it)) {
        memset(sc.name, 0, sizeof(sc.name));
        strncpy(sc.name, it->valuestring, sizeof(sc.name) - 1);
    }
    auto num = [&](const char* k, double lo, double hi, double* out) {
        it = cJSON_GetObjectItemCaseSensitive(js, k);
        if (cJSON_IsNumber(it) && it->valuedouble >= lo && it->valuedouble <= hi) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    double v;
    if (num("effect", 0, 2, &v)) sc.effect = static_cast<uint8_t>(v);
    if (num("speed", 0, 255, &v)) sc.speed = static_cast<uint8_t>(v);
    if (num("param", 0, 255, &v)) sc.param = static_cast<uint8_t>(v);
    if (num("mask", 0, 255, &v)) sc.channel_mask = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(js, "color");
    if (cJSON_IsString(it)) {
        unsigned cr, cg, cb;
        if (sscanf(it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring,
                   "%02x%02x%02x", &cr, &cg, &cb) == 3) {
            sc.r = static_cast<uint8_t>(cr);
            sc.g = static_cast<uint8_t>(cg);
            sc.b = static_cast<uint8_t>(cb);
        }
    }
    config::set_scene(i, sc);
}

static esp_err_t handle_restore(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    static char buf[4096];  // full backup ≈ 3 kB; static keeps it off the httpd stack
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    cJSON* jg = cJSON_GetObjectItemCaseSensitive(j, "global");
    if (cJSON_IsObject(jg)) restore_global(jg);
    cJSON* jchs = cJSON_GetObjectItemCaseSensitive(j, "channels");
    if (cJSON_IsArray(jchs)) {
        const int n = cJSON_GetArraySize(jchs);
        for (int i = 0; i < n && i < static_cast<int>(config::kNumChannels); ++i)
            restore_channel(static_cast<size_t>(i), cJSON_GetArrayItem(jchs, i));
    }
    cJSON* jsc = cJSON_GetObjectItemCaseSensitive(j, "scenes");
    if (cJSON_IsArray(jsc)) {
        const int n = cJSON_GetArraySize(jsc);
        for (int i = 0; i < n && i < static_cast<int>(config::kNumScenes); ++i)
            restore_scene(static_cast<size_t>(i), cJSON_GetArrayItem(jsc, i));
    }
    cJSON_Delete(j);
    dmx::mark_global_dirty();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":true,\"note\":\"network/web changes apply after reboot\"}");
}

// ── POST /api/global ─────────────────────────────────────────────────────────

static esp_err_t handle_post_global(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[512];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");

    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::GlobalConfig g = config::get_global();
    bool network_changed   = false;

    auto get_bool = [&](const char* key, bool& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item) return false;
        if (cJSON_IsBool(item)) {
            out = cJSON_IsTrue(item);
            return true;
        }
        if (cJSON_IsNumber(item)) {
            out = (item->valuedouble != 0);
            return true;
        }
        return false;
    };
    auto get_u32 = [&](const char* key, uint32_t lo, uint32_t hi, uint32_t& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsNumber(item)) return false;
        const uint32_t v = static_cast<uint32_t>(item->valuedouble);
        if (v < lo || v > hi) return false;
        out = v;
        return true;
    };
    auto get_str = [&](const char* key) -> const char* {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsString(item)) return nullptr;
        return item->valuestring;
    };

    uint32_t u = 0;
    bool b     = false;
    const char* s;

    if (get_bool("dhcp", b)) {
        g.use_dhcp      = b;
        network_changed = true;
    }
    if ((s = get_str("ip"))) {
        if (!parse_ip(s, g.static_ip)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad ip");
        }
        network_changed = true;
    }
    if ((s = get_str("mask"))) {
        if (!parse_ip(s, g.static_mask)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad mask");
        }
        network_changed = true;
    }
    if ((s = get_str("gw"))) {
        if (!parse_ip(s, g.static_gateway)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad gw");
        }
        network_changed = true;
    }
    if (get_u32("net", 0, 127, u)) g.artnet_net = static_cast<uint8_t>(u);
    if (get_u32("subnet", 0, 15, u)) g.artnet_subnet = static_cast<uint8_t>(u);
    if ((s = get_str("short_name"))) {
        memset(g.short_name, 0, sizeof(g.short_name));
        strncpy(g.short_name, s, sizeof(g.short_name) - 1);
    }
    if ((s = get_str("long_name"))) {
        memset(g.long_name, 0, sizeof(g.long_name));
        strncpy(g.long_name, s, sizeof(g.long_name) - 1);
    }
    if (get_bool("reply_unicast", b)) g.artnet_poll_reply_unicast = b;
    {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, "refresh_hz");
        if (item && cJSON_IsNumber(item)) {
            const uint32_t v = static_cast<uint32_t>(item->valuedouble);
            if (v == 30 || v == 60) g.refresh_rate_hz = static_cast<uint8_t>(v);
        }
    }
    if (get_u32("home_timeout_s", 0, 65535, u)) g.home_timeout_s = static_cast<uint16_t>(u);
    if (get_bool("web_enabled", b)) g.web_enabled = b;
    if (get_u32("failsafe_mode", 0, 3, u)) g.failsafe_mode = static_cast<uint8_t>(u);
    if (get_u32("failsafe_scene", 0, config::kNumScenes - 1, u))
        g.failsafe_scene = static_cast<uint8_t>(u);
    if (get_u32("boot_scene", 0, config::kNumScenes, u)) g.boot_scene = static_cast<uint8_t>(u);
    if (get_u32("failsafe_timeout_s", 0, 3600, u)) g.failsafe_timeout_s = static_cast<uint16_t>(u);
    if (get_u32("merge_mode", 0, 1, u)) g.merge_mode = static_cast<uint8_t>(u);
    if ((s = get_str("failsafe_color"))) {
        unsigned fr, fg, fb;
        if (sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &fr, &fg, &fb) == 3) {
            g.failsafe_r = static_cast<uint8_t>(fr);
            g.failsafe_g = static_cast<uint8_t>(fg);
            g.failsafe_b = static_cast<uint8_t>(fb);
        }
    }
    bool sacn_changed = false;
    if (get_bool("sacn_enabled", b)) {
        sacn_changed   = (g.sacn_enabled != b);
        g.sacn_enabled = b;
    }
    bool fpp_changed = false;
    if (get_bool("fpp_remote", b)) {
        fpp_changed  = (g.fpp_remote != b);
        g.fpp_remote = b;
    }

    // Admin password: separate setter (hashes + persists on its own); empty
    // string clears it (auth off). Copied out before cJSON_Delete frees the
    // backing buffer. Never echoed back.
    char pwd[64];
    bool password_changed = false;
    if ((s = get_str("web_password"))) {
        strncpy(pwd, s, sizeof(pwd) - 1);
        pwd[sizeof(pwd) - 1] = '\0';
        password_changed     = true;
    }

    cJSON_Delete(j);
    config::set_global(g);
    if (password_changed) config::set_web_password(pwd);
    dmx::mark_global_dirty();

    if (sacn_changed) {
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
    }
    if (fpp_changed) {
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed)
        cJSON_AddStringToObject(resp, "note", "network_changes_apply_after_reboot");
    return send_json(req, resp);
}

// ── POST /api/channel/{n} ────────────────────────────────────────────────────

static esp_err_t handle_post_channel(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // URI: /api/channel/N or /api/channel/N/identify
    const char* tail = req->uri + strlen("/api/channel/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::kNumChannels)
        return send_err(req, 400, "channel 0..7");
    if (strstr(tail, "/identify") != nullptr) {
        dmx::identify_start(static_cast<size_t>(idx));
        return send_ok(req);
    }

    char buf[512];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");

    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::ChannelConfig c = config::get_channel(static_cast<size_t>(idx));

    auto get_u32 = [&](const char* key, uint32_t lo, uint32_t hi, uint32_t& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsNumber(item)) return false;
        const uint32_t v = static_cast<uint32_t>(item->valuedouble);
        if (v < lo || v > hi) return false;
        out = v;
        return true;
    };
    auto get_bool = [&](const char* key, bool& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item) return false;
        if (cJSON_IsBool(item)) {
            out = cJSON_IsTrue(item);
            return true;
        }
        if (cJSON_IsNumber(item)) {
            out = (item->valuedouble != 0);
            return true;
        }
        return false;
    };
    auto get_str = [&](const char* key) -> const char* {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsString(item)) return nullptr;
        return item->valuestring;
    };

    uint32_t u = 0;
    bool b     = false;
    const char* s;

    if ((s = get_str("protocol"))) {
        const int p = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT), s);
        if (p < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad protocol");
        }
        c.protocol = static_cast<led::Protocol>(p);
    }
    if ((s = get_str("color_order"))) {
        const int o = lookup(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT), s);
        if (o < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad color_order");
        }
        c.color_order = static_cast<led::ColorOrder>(o);
    }
    if (get_u32("universe_start", 1, 32767, u)) c.universe_start = static_cast<uint16_t>(u);
    if (get_u32("dmx_start", 1, 512, u)) c.dmx_start = static_cast<uint16_t>(u);
    if (get_u32("pixel_count", 1, dmx::kMaxPixelsPerChan, u))
        c.pixel_count = static_cast<uint16_t>(u);
    if (get_u32("brightness", 0, 255, u)) c.brightness = static_cast<uint8_t>(u);
    if (get_u32("grouping", 1, 8, u)) c.grouping = static_cast<uint8_t>(u);
    if (get_bool("invert", b)) c.invert_direction = b;
    if (get_u32("clock_hz", 100'000, led::kPclkHz / 2, u)) c.clock_hz = u;
    if (get_u32("gamma_x10", 10, 40, u)) c.gamma_x10 = static_cast<uint8_t>(u);
    if ((s = get_str("wb"))) {
        unsigned wr, wg, wbv;
        if (sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &wr, &wg, &wbv) == 3) {
            c.wb_r = wr ? static_cast<uint8_t>(wr) : 255;
            c.wb_g = wg ? static_cast<uint8_t>(wg) : 255;
            c.wb_b = wbv ? static_cast<uint8_t>(wbv) : 255;
        }
    }

    cJSON_Delete(j);
    config::set_channel(static_cast<size_t>(idx), c);
    dmx::mark_channel_dirty(static_cast<size_t>(idx));
    return send_ok(req);
}

// ── POST /api/ota ────────────────────────────────────────────────────────────
// Raw firmware binary in the request body → inactive OTA slot. esp_ota_end()
// validates the image (magic, chip, SHA); on success the boot partition is
// switched and the device restarts. With BOOTLOADER_APP_ROLLBACK_ENABLE the
// new image must confirm itself at boot-complete or the bootloader rolls
// back to the current slot — a power cut mid-flash is also safe (the running
// slot is never touched).

static bool g_ota_in_progress = false;

static esp_err_t handle_ota(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (g_ota_in_progress) return send_err(req, 500, "OTA already in progress");

    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) return send_err(req, 500, "no OTA partition (single-app table?)");

    const int total = req->content_len;
    if (total <= 0) return send_err(req, 400, "empty body");
    if (static_cast<size_t>(total) > update->size) return send_err(req, 400, "image too large");

    g_ota_in_progress = true;
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", total, update->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err        = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_err(req, 500, "esp_ota_begin failed");
    }

    // Static buffer: the httpd task stack is small and only one OTA can run
    // at a time (guarded by g_ota_in_progress).
    static char buf[4096];
    int remaining = total;
    while (remaining > 0) {
        const int n = httpd_req_recv(
            req, buf,
            remaining < static_cast<int>(sizeof(buf)) ? remaining : static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            esp_ota_abort(ota);
            g_ota_in_progress = false;
            return send_err(req, 400, "upload interrupted");
        }
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            g_ota_in_progress = false;
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return send_err(req, 500, "flash write failed");
        }
        remaining -= n;
    }

    err = esp_ota_end(ota);  // validates magic / chip / image integrity
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_err(req, 400, "image validation failed");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        g_ota_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return send_err(req, 500, "set boot partition failed");
    }

    ESP_LOGI(TAG, "OTA complete, rebooting into %s", update->label);
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"partition\":\"%s\",\"rebooting\":true}",
             update->label);
    httpd_resp_sendstr(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));  // let the response flush
    esp_restart();
    return ESP_OK;
}

// ── POST /api/scene/{n} (config) + /api/scene/{n}/play + /api/scenes/stop ───

static esp_err_t handle_post_scene(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;

    // URI: /api/scene/N or /api/scene/N/play
    const char* tail = req->uri + strlen("/api/scene/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::kNumScenes)
        return send_err(req, 400, "scene 0..7");
    const bool play = strstr(tail, "/play") != nullptr;

    if (play) {
        dmx::scene_start(static_cast<uint8_t>(idx));
        return send_ok(req);
    }

    char buf[256];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    auto sc = config::get_scene(static_cast<size_t>(idx));

    cJSON* item = cJSON_GetObjectItemCaseSensitive(j, "name");
    if (cJSON_IsString(item)) {
        memset(sc.name, 0, sizeof(sc.name));
        strncpy(sc.name, item->valuestring, sizeof(sc.name) - 1);
    }
    item = cJSON_GetObjectItemCaseSensitive(j, "effect");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 2)
        sc.effect = static_cast<uint8_t>(item->valuedouble);
    item = cJSON_GetObjectItemCaseSensitive(j, "color");
    if (cJSON_IsString(item)) {
        const char* cs = item->valuestring;
        unsigned r, gg, b;
        if (sscanf(cs[0] == '#' ? cs + 1 : cs, "%02x%02x%02x", &r, &gg, &b) == 3) {
            sc.r = static_cast<uint8_t>(r);
            sc.g = static_cast<uint8_t>(gg);
            sc.b = static_cast<uint8_t>(b);
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(j, "speed");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 255)
        sc.speed = static_cast<uint8_t>(item->valuedouble);
    item = cJSON_GetObjectItemCaseSensitive(j, "param");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 255)
        sc.param = static_cast<uint8_t>(item->valuedouble);
    item = cJSON_GetObjectItemCaseSensitive(j, "mask");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 255)
        sc.channel_mask = static_cast<uint8_t>(item->valuedouble);

    cJSON_Delete(j);
    config::set_scene(static_cast<size_t>(idx), sc);
    return send_ok(req);
}

static esp_err_t handle_scenes_stop(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    dmx::scene_stop();
    return send_ok(req);
}

// ── POST /api/reboot ─────────────────────────────────────────────────────────

static esp_err_t handle_reboot(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

// ── POST /api/factory-reset ──────────────────────────────────────────────────

// ── GET /api/fseq/files ──────────────────────────────────────────────────────

static esp_err_t handle_fseq_files(httpd_req_t* req) {
    char names[fseq::kMaxFiles][fseq::kMaxNameLen];
    const size_t n = fseq::list_files(names, fseq::kMaxFiles);
    cJSON* arr     = cJSON_CreateArray();
    for (size_t i = 0; i < n; ++i)
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "files", arr);
    const char* playing = fseq::active_file();
    cJSON_AddStringToObject(root, "active", playing ? playing : "");
    return send_json(req, root);
}

// ── POST /api/fseq/play ──────────────────────────────────────────────────────

static esp_err_t handle_fseq_play(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char body[fseq::kMaxNameLen + 32];
    const int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return send_err(req, 400, "Empty body");
    body[len]   = '\0';
    cJSON* root = cJSON_Parse(body);
    if (!root) return send_err(req, 400, "Invalid JSON");
    cJSON* fn = cJSON_GetObjectItemCaseSensitive(root, "filename");
    if (!cJSON_IsString(fn) || !fn->valuestring || !fn->valuestring[0]) {
        cJSON_Delete(root);
        return send_err(req, 400, "Missing filename");
    }
    char filename[fseq::kMaxNameLen];
    strncpy(filename, fn->valuestring, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    cJSON_Delete(root);
    if (!fseq::start(filename)) return send_err(req, 500, fseq::error_string());
    return send_ok(req);
}

// ── POST /api/fseq/stop ──────────────────────────────────────────────────────

static esp_err_t handle_fseq_stop(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    fseq::stop();
    return send_ok(req);
}

// ── POST /api/fseq/upload?name=<file.fseq> ──────────────────────────────────
// Raw body streamed to the SD card. The data lands in a .part temp file
// first and is renamed on success, so an interrupted upload never leaves a
// truncated .fseq visible to the player.

static bool g_fseq_upload_in_progress = false;

// Decode %XX sequences in-place (httpd_query_key_value does not URL-decode).
static void url_decode(char* s) {
    char* w = s;
    for (; *s; ++w) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3]  = { s[1], s[2], 0 };
            *w           = static_cast<char>(strtol(hex, nullptr, 16));
            s           += 3;
        } else {
            *w = (*s == '+') ? ' ' : *s;
            ++s;
        }
    }
    *w = '\0';
}

static esp_err_t handle_fseq_upload(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (g_fseq_upload_in_progress) return send_err(req, 500, "upload already in progress");
    if (fseq::sd_state() != fseq::SdState::Mounted) return send_err(req, 500, "no SD card");

    char query[fseq::kMaxNameLen * 3 + 16] = {};
    char name[fseq::kMaxNameLen]           = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !name[0])
        return send_err(req, 400, "missing ?name=<file.fseq>");
    url_decode(name);

    if (strchr(name, '/') || strchr(name, '\\') || name[0] == '.')
        return send_err(req, 400, "bad filename");
    const size_t nl = strlen(name);
    if (nl < 6 || strcasecmp(name + nl - 5, ".fseq") != 0)
        return send_err(req, 400, "filename must end in .fseq");

    const int total = req->content_len;
    if (total <= 0) return send_err(req, 400, "empty body");

    // Overwriting the file currently playing: stop playback to release it.
    const char* active = fseq::active_file();
    if (active && strcasecmp(active, name) == 0) fseq::stop();

    char tmp_path[96];
    char path[96 + fseq::kMaxNameLen];
    snprintf(tmp_path, sizeof(tmp_path), "%s/upload.part", fseq::kMountPath);
    snprintf(path, sizeof(path), "%s/%s", fseq::kMountPath, name);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return send_err(req, 500, "cannot create file on SD");
    g_fseq_upload_in_progress = true;
    ESP_LOGI(TAG, "FSEQ upload: %d bytes -> %s", total, name);

    // Static buffer: the httpd task stack is small and only one upload can
    // run at a time (guarded by g_fseq_upload_in_progress).
    static char buf[4096];
    int remaining = total;
    while (remaining > 0) {
        const int n = httpd_req_recv(
            req, buf,
            remaining < static_cast<int>(sizeof(buf)) ? remaining : static_cast<int>(sizeof(buf)));
        if (n <= 0 || fwrite(buf, 1, n, fp) != static_cast<size_t>(n)) {
            fclose(fp);
            remove(tmp_path);
            g_fseq_upload_in_progress = false;
            return send_err(req, n <= 0 ? 400 : 500,
                            n <= 0 ? "upload interrupted" : "SD write failed (card full?)");
        }
        remaining -= n;
    }
    fclose(fp);

    remove(path);  // FATFS rename does not overwrite
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        g_fseq_upload_in_progress = false;
        return send_err(req, 500, "rename failed");
    }
    g_fseq_upload_in_progress = false;
    ESP_LOGI(TAG, "FSEQ upload complete: %s", name);
    return send_ok(req);
}

static esp_err_t handle_factory_reset(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    config::reset_to_defaults();
    dmx::mark_global_dirty();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        dmx::mark_channel_dirty(ch);
    return send_ok(req);
}

// ── mDNS ────────────────────────────────────────────────────────────────────
// pixfrog.local, advertised only while the web UI is enabled — mDNS rides
// the web_enabled opt-in, no extra flag. Instance name = the ArtNet short
// name so several boxes are distinguishable in a browser.

static bool g_mdns_up = false;

static void start_mdns() {
    if (g_mdns_up) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    mdns_hostname_set("pixfrog");
    mdns_instance_name_set(config::get_global().short_name);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    g_mdns_up = true;
    ESP_LOGI(TAG, "mDNS: pixfrog.local");
}

static void stop_mdns() {
    if (!g_mdns_up) return;
    mdns_free();
    g_mdns_up = false;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

void start() {
    if (g_server) return;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 8192;  // esp_ota_* calls need headroom over the 4 kB default

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        g_server = nullptr;
        return;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = nullptr },
        { .uri      = "/api/config",
          .method   = HTTP_GET,
          .handler  = handle_get_config,
          .user_ctx = nullptr },
        { .uri      = "/api/global",
          .method   = HTTP_POST,
          .handler  = handle_post_global,
          .user_ctx = nullptr },
        { .uri      = "/api/channel/*",
          .method   = HTTP_POST,
          .handler  = handle_post_channel,
          .user_ctx = nullptr },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota, .user_ctx = nullptr },
        { .uri = "/api/backup", .method = HTTP_GET, .handler = handle_backup, .user_ctx = nullptr },
        { .uri      = "/api/restore",
          .method   = HTTP_POST,
          .handler  = handle_restore,
          .user_ctx = nullptr },
        { .uri      = "/api/scene/*",
          .method   = HTTP_POST,
          .handler  = handle_post_scene,
          .user_ctx = nullptr },
        { .uri      = "/api/scenes/stop",
          .method   = HTTP_POST,
          .handler  = handle_scenes_stop,
          .user_ctx = nullptr },
        { .uri      = "/api/reboot",
          .method   = HTTP_POST,
          .handler  = handle_reboot,
          .user_ctx = nullptr },
        { .uri      = "/api/factory-reset",
          .method   = HTTP_POST,
          .handler  = handle_factory_reset,
          .user_ctx = nullptr },
        { .uri      = "/api/fseq/files",
          .method   = HTTP_GET,
          .handler  = handle_fseq_files,
          .user_ctx = nullptr },
        { .uri      = "/api/fseq/play",
          .method   = HTTP_POST,
          .handler  = handle_fseq_play,
          .user_ctx = nullptr },
        { .uri      = "/api/fseq/stop",
          .method   = HTTP_POST,
          .handler  = handle_fseq_stop,
          .user_ctx = nullptr },
        { .uri      = "/api/fseq/upload",
          .method   = HTTP_POST,
          .handler  = handle_fseq_upload,
          .user_ctx = nullptr },
        { .uri      = "/api/status",
          .method   = HTTP_GET,
          .handler  = handle_get_status,
          .user_ctx = nullptr },
        { .uri      = "/api/coredump",
          .method   = HTTP_GET,
          .handler  = handle_coredump_get,
          .user_ctx = nullptr },
        { .uri      = "/api/coredump",
          .method   = HTTP_DELETE,
          .handler  = handle_coredump_delete,
          .user_ctx = nullptr },
    };
    for (const auto& r : routes)
        httpd_register_uri_handler(g_server, &r);

    ESP_LOGI(TAG, "HTTP server started on port %u", cfg.server_port);
    start_mdns();
}

void stop() {
    if (!g_server) return;
    httpd_stop(g_server);
    g_server = nullptr;
    stop_mdns();
    ESP_LOGI(TAG, "HTTP server stopped");
}

bool is_running() {
    return g_server != nullptr;
}

}  // namespace pixfrog::web
