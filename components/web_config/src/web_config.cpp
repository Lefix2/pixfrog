#include "web_config.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"
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

static esp_err_t handle_get_config(httpd_req_t* req) {
    const auto& g = config::get_global();

    cJSON* root = cJSON_CreateObject();

    // Status (live)
    cJSON_AddBoolToObject(root, "link", ui::is_link_up());
    cJSON_AddNumberToObject(root, "fps", static_cast<double>(dmx::get_stats().current_fps));
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);

    // Global config
    cJSON* jg = cJSON_AddObjectToObject(root, "global");
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

    // Channels
    cJSON* jchs = cJSON_AddArrayToObject(root, "channels");
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
        cJSON_AddItemToArray(jchs, jc);
    }

    return send_json(req, root);
}

// ── POST /api/global ─────────────────────────────────────────────────────────

static esp_err_t handle_post_global(httpd_req_t* req) {
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

    cJSON_Delete(j);
    config::set_global(g);
    dmx::mark_global_dirty();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed)
        cJSON_AddStringToObject(resp, "note", "network_changes_apply_after_reboot");
    return send_json(req, resp);
}

// ── POST /api/channel/{n} ────────────────────────────────────────────────────

static esp_err_t handle_post_channel(httpd_req_t* req) {
    // Extract channel index from URI (/api/channel/N)
    const char* uri        = req->uri;
    const char* last_slash = strrchr(uri, '/');
    if (!last_slash) return send_err(req, 400, "bad URI");
    const int idx = atoi(last_slash + 1);
    if (idx < 0 || static_cast<size_t>(idx) >= config::kNumChannels)
        return send_err(req, 400, "channel 0..7");

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

    cJSON_Delete(j);
    config::set_channel(static_cast<size_t>(idx), c);
    dmx::mark_channel_dirty(static_cast<size_t>(idx));
    return send_ok(req);
}

// ── POST /api/reboot ─────────────────────────────────────────────────────────

static esp_err_t handle_reboot(httpd_req_t* req) {
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

// ── POST /api/factory-reset ──────────────────────────────────────────────────

static esp_err_t handle_factory_reset(httpd_req_t* req) {
    config::reset_to_defaults();
    dmx::mark_global_dirty();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        dmx::mark_channel_dirty(ch);
    return send_ok(req);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

void start() {
    if (g_server) return;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

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
        { .uri      = "/api/reboot",
          .method   = HTTP_POST,
          .handler  = handle_reboot,
          .user_ctx = nullptr },
        { .uri      = "/api/factory-reset",
          .method   = HTTP_POST,
          .handler  = handle_factory_reset,
          .user_ctx = nullptr },
    };
    for (const auto& r : routes)
        httpd_register_uri_handler(g_server, &r);

    ESP_LOGI(TAG, "HTTP server started on port %u", cfg.server_port);
}

void stop() {
    if (!g_server) return;
    httpd_stop(g_server);
    g_server = nullptr;
    ESP_LOGI(TAG, "HTTP server stopped");
}

bool is_running() {
    return g_server != nullptr;
}

}  // namespace pixfrog::web
