// Handler plumbing: the log capture ring, the OTA rollback record, JSON/body
// helpers, the auth and cross-origin gates, protocol name tables.

#include "web_internal.h"

namespace pixfrog::web::impl {

httpd_handle_t g_server = nullptr;

// ── Log capture ring ────────────────────────────────────────────────────────
// A vprintf tee on esp_log: every formatted log line also lands in a fixed ring
// so the Diagnostics tab can read recent logs over HTTP (GET /api/logs). The
// original sink (UART) is preserved. The hook runs in task context — esp_log is
// not called from ISRs here — so a short spinlock around the ring is enough.

char g_log_ring[kLogRing];
size_t g_log_head             = 0;  // next write position
bool g_log_wrapped            = false;
portMUX_TYPE g_log_mux        = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_prev_vprintf = nullptr;

int log_vprintf(const char* fmt, va_list ap) {
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    // Demote the IDF SD mount-retry spam to debug: with no microSD the
    // sdmmc/vfs_fat drivers emit an ERROR line every second. Below debug
    // verbosity, keep it out of the log entirely; at debug, relabel its 'E'
    // severity to 'D' in the ring so the Diagnostics panel shows it as debug.
    if (n > 0 && strstr(line, "sdmmc")) {
        if (esp_log_level_get("*") < ESP_LOG_DEBUG) return n;
        char* sev = strstr(line, "E (");
        if (sev) *sev = 'D';
    }
    if (n > 0) {
        const size_t len = (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n)
                                                                   : sizeof(line) - 1;
        portENTER_CRITICAL(&g_log_mux);
        for (size_t i = 0; i < len; ++i) {
            if (line[i] == '\x1b') {  // drop the "ESC [ … m" colour escapes
                while (i < len && line[i] != 'm')
                    ++i;
                continue;
            }
            g_log_ring[g_log_head++] = line[i];
            if (g_log_head == kLogRing) {
                g_log_head    = 0;
                g_log_wrapped = true;
            }
        }
        portEXIT_CRITICAL(&g_log_mux);
    }
    return g_prev_vprintf ? g_prev_vprintf(fmt, ap) : n;  // tee to UART
}

const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int-wdt";
    case ESP_RST_TASK_WDT: return "task-wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

config::RollbackRecord g_rollback{};

const config::RollbackRecord* rollback_record() {
    return config::get_rollback(g_rollback) ? &g_rollback : nullptr;
}

cJSON* rollback_json(const config::RollbackRecord& r) {
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "rejected_version", r.rejected_version);
    cJSON_AddStringToObject(j, "rejected_slot", r.rejected_slot);
    cJSON_AddStringToObject(j, "running_version", r.running_version);
    cJSON_AddStringToObject(j, "reset_reason",
                            reset_reason_str(static_cast<esp_reset_reason_t>(r.reset_reason)));
    cJSON_AddBoolToObject(j, "acknowledged", r.acknowledged != 0);
    return j;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

void fmt_ip(char* buf, size_t cap, uint32_t ip) {
    snprintf(buf, cap, "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
             static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
             static_cast<unsigned>(ip & 0xFFu));
}

bool parse_ip(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

// Read request body (up to max_len bytes). Returns actual length, or 0 on error.
size_t read_body(httpd_req_t* req, char* buf, size_t max_len) {
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

esp_err_t send_json(httpd_req_t* req, cJSON* root) {
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    // CORS: the aggregated multi-node dashboard reads /api/config + /api/status
    // from sibling pixfrogs cross-origin. These are simple read-only GETs (no
    // custom headers → no preflight); ACAO:* is enough. Writes stay protected
    // by Basic auth, so opening reads carries no extra mutation surface.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return r;
}

esp_err_t send_ok(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t send_err(httpd_req_t* req, int code, const char* msg) {
    const char* status = "500 Internal Server Error";
    if (code == 400) status = "400 Bad Request";
    if (code == 404) status = "404 Not Found";
    if (code == 409) status = "409 Conflict";
    httpd_resp_set_status(req, status);
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

    // Room for "Basic " + base64 of a 128-byte "user:password": the longest
    // password (kMaxWebPasswordLen) with any reasonable user name.
    char header[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        return false;
    if (strncmp(header, "Basic ", 6) != 0) return false;

    unsigned char decoded[136];
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

// ── Cross-origin write gate ─────────────────────────────────────────────────
// Every mutation here is a CORS "simple request": a browser will send it
// cross-origin with no preflight, so any page a LAN user visits could POST to
// this device. Basic auth does not stop that on its own — it is off until an
// admin password is set, which is not the default.
//
// Browsers always attach `Origin` to a cross-origin write, so requiring it to
// match our own `Host` closes the class outright. Non-browser clients (curl,
// uartctl, the OTA uploader) send no Origin at all and are unaffected; the
// aggregated multi-node dashboard only ever reads cross-origin, and GETs do
// not come through here.
static bool same_origin(httpd_req_t* req) {
    char origin[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
        return true;  // no Origin: not a browser-initiated cross-site write

    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;

    // Origin is "scheme://host[:port]"; compare the authority against Host.
    const char* sep = strstr(origin, "://");
    if (!sep) return false;
    return strcmp(sep + 3, host) == 0;
}

// Returns true when the request may proceed; otherwise the error has been sent.
bool require_auth(httpd_req_t* req) {
    if (!same_origin(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"cross-origin write rejected\"}");
        return false;
    }
    if (authorized(req)) return true;
    vTaskDelay(pdMS_TO_TICKS(500));  // flat anti-brute-force cost
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"pixfrog\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return false;
}

const char* const kProtoNames[] = { "Off",    "WS2815", "WS2812B", "WS2811", "SK6812",
                                    "WS2814", "APA102", "SK9822",  "LPD8806" };
static_assert(sizeof(kProtoNames) / sizeof(kProtoNames[0]) ==
              static_cast<size_t>(led::Protocol::COUNT));

const char* const kOrderNames[] = { "RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW" };
static_assert(sizeof(kOrderNames) / sizeof(kOrderNames[0]) ==
              static_cast<size_t>(led::ColorOrder::COUNT));

int lookup(const char* const* names, size_t count, const char* s) {
    for (size_t i = 0; i < count; ++i)
        if (strcasecmp(names[i], s) == 0) return static_cast<int>(i);
    return -1;
}

}  // namespace pixfrog::web::impl
