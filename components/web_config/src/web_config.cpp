#include "web_config.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mdns.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_output.h"
#include "led_protocols.h"
#include "sacn.h"
#include "ui.h"

namespace pixfrog::web {

namespace {

constexpr const char* TAG = "WEB";

httpd_handle_t g_server = nullptr;

// Embedded SPA (web_ui.html baked in at link time).
extern const uint8_t web_ui_gz_start[] asm("_binary_web_ui_html_gz_start");
extern const uint8_t web_ui_gz_end[] asm("_binary_web_ui_html_gz_end");

// ── Log capture ring ────────────────────────────────────────────────────────
// A vprintf tee on esp_log: every formatted log line also lands in a fixed ring
// so the Diagnostics tab can read recent logs over HTTP (GET /api/logs). The
// original sink (UART) is preserved. The hook runs in task context — esp_log is
// not called from ISRs here — so a short spinlock around the ring is enough.

constexpr size_t kLogRing = 8192;
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

// ── OTA rollback record ─────────────────────────────────────────────────────
// Served by /api/status (banner while unacknowledged) and /api/diag (history).
// Re-read on each request (one small NVS blob) so an acknowledge from the
// console clears the web banner too. httpd is single-threaded: one buffer.
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
    // CORS: the aggregated multi-node dashboard reads /api/config + /api/status
    // from sibling pixfrogs cross-origin. These are simple read-only GETs (no
    // custom headers → no preflight); ACAO:* is enough. Writes stay protected
    // by Basic auth, so opening reads carries no extra mutation surface.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return r;
}

static esp_err_t send_ok(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_err(httpd_req_t* req, int code, const char* msg) {
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
static bool require_auth(httpd_req_t* req) {
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

// ── Protocol / color-order name tables ──────────────────────────────────────

static const char* const kProtoNames[] = { "Off",    "WS2815", "WS2812B", "WS2811", "SK6812",
                                           "WS2814", "APA102", "SK9822",  "LPD8806" };
static_assert(sizeof(kProtoNames) / sizeof(kProtoNames[0]) ==
              static_cast<size_t>(led::Protocol::COUNT));

static const char* const kOrderNames[] = {
    "RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW"
};
static_assert(sizeof(kOrderNames) / sizeof(kOrderNames[0]) ==
              static_cast<size_t>(led::ColorOrder::COUNT));

static int lookup(const char* const* names, size_t count, const char* s) {
    for (size_t i = 0; i < count; ++i)
        if (strcasecmp(names[i], s) == 0) return static_cast<int>(i);
    return -1;
}

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
static esp_err_t handle_root(httpd_req_t* req) {
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

// ── DMX control universe JSON ───────────────────────────────────────────────
// {"enabled":true,"universe":100,"address":1,"footprint":6,
//  "slots":[{"fn":"master","mask":255,"index":0,"fine":true}, ...]}

static cJSON* build_playlist_json();

static cJSON* build_control_json() {
    const auto& c = config::get_control();
    cJSON* jc     = cJSON_CreateObject();
    cJSON_AddBoolToObject(jc, "enabled", c.enabled);
    cJSON_AddNumberToObject(jc, "universe", c.universe);
    cJSON_AddNumberToObject(jc, "address", c.address);
    cJSON_AddNumberToObject(jc, "footprint", static_cast<double>(config::control_footprint(c)));
    cJSON* js = cJSON_CreateArray();
    for (size_t i = 0; i < c.count; ++i) {
        const auto& sl = c.slots[i];
        cJSON* j       = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "fn", config::ctl_fn_id(sl.fn));
        cJSON_AddNumberToObject(j, "mask", sl.mask);
        cJSON_AddNumberToObject(j, "index", sl.index);
        cJSON_AddBoolToObject(j, "fine", (sl.flags & config::kCtlFlagFine) != 0);
        cJSON_AddItemToArray(js, j);
    }
    cJSON_AddItemToObject(jc, "slots", js);
    return jc;
}

// Applies the fields present in `jc` onto `c`. False (with *why) on anything
// out of range: nothing is half-applied by the caller then.
static bool apply_control_json(const cJSON* jc, config::ControlConfig& c, const char** why) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(jc, "enabled");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "enabled: boolean", false;
        c.enabled = cJSON_IsTrue(it) ? 1 : 0;
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "universe");
    if (it) {
        if (!cJSON_IsNumber(it) || !(it->valuedouble >= 0 && it->valuedouble <= 32767))
            return *why = "universe: 0..32767", false;
        c.universe = static_cast<uint16_t>(it->valuedouble);
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "address");
    if (it) {
        if (!cJSON_IsNumber(it) || !(it->valuedouble >= 1 && it->valuedouble <= 512))
            return *why = "address: 1..512", false;
        c.address = static_cast<uint16_t>(it->valuedouble);
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "preset");
    if (it) {
        if (cJSON_IsString(it) && strcmp(it->valuestring, "simple") == 0)
            config::control_apply_preset(c, config::ControlPreset::Simple);
        else if (cJSON_IsString(it) && strcmp(it->valuestring, "full") == 0)
            config::control_apply_preset(c, config::ControlPreset::Full);
        else
            return *why = "preset: simple|full", false;
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "slots");
    if (it) {
        if (!cJSON_IsArray(it) ||
            cJSON_GetArraySize(it) > static_cast<int>(config::kMaxControlSlots))
            return *why = "slots: array of at most 32", false;
        config::ControlSlot parsed[config::kMaxControlSlots]{};
        size_t n = 0;
        for (const cJSON* js = it->child; js; js = js->next) {
            const cJSON* fn = cJSON_GetObjectItemCaseSensitive(js, "fn");
            const int f     = cJSON_IsString(fn) ? config::ctl_fn_from_id(fn->valuestring) : -1;
            if (f < 0) return *why = "slots[].fn: unknown function", false;
            config::ControlSlot sl = config::control_slot(static_cast<config::CtlFn>(f));
            const cJSON* m         = cJSON_GetObjectItemCaseSensitive(js, "mask");
            if (m) {
                if (!cJSON_IsNumber(m) || !(m->valuedouble >= 1 && m->valuedouble <= 255))
                    return *why = "slots[].mask: 1..255", false;
                sl.mask = static_cast<uint8_t>(m->valuedouble);
            }
            const cJSON* ix = cJSON_GetObjectItemCaseSensitive(js, "index");
            if (ix) {
                if (!cJSON_IsNumber(ix) ||
                    !(ix->valuedouble >= 0 && ix->valuedouble <= config::kSceneColorsMax - 1))
                    return *why = "slots[].index: 0..3", false;
                sl.index = static_cast<uint8_t>(ix->valuedouble);
            }
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(js, "fine")) &&
                f == static_cast<int>(config::CtlFn::Master))
                sl.flags = config::kCtlFlagFine;
            parsed[n++] = sl;
        }
        std::memcpy(c.slots, parsed, sizeof(c.slots));
        c.count = static_cast<uint8_t>(n);
    }
    config::ControlConfig check = c;
    config::sanitize_control(check);
    if (check.count < c.count) return *why = "the mode would end past DMX channel 512", false;
    return true;
}

// ── OFL fixture profile of the control mode ─────────────────────────────────
// Open Fixture Library format (importable in QLC+, and convertible to most
// desk formats on open-fixture-library.org): one mode, the slots in order.

static void ofl_range(cJSON* caps, int lo, int hi, cJSON* cap) {
    cJSON* r = cJSON_CreateArray();
    cJSON_AddItemToArray(r, cJSON_CreateNumber(lo));
    cJSON_AddItemToArray(r, cJSON_CreateNumber(hi));
    cJSON_AddItemToObject(cap, "dmxRange", r);
    cJSON_AddItemToArray(caps, cap);
}

static cJSON* ofl_cap(const char* type) {
    cJSON* c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", type);
    return c;
}

// "Master", or "Master (out 1-4)" when the slot does not cover every output.
static void ofl_channel_name(char* out, size_t cap, const char* base, uint8_t mask) {
    if (mask == 0xFF) {
        snprintf(out, cap, "%s", base);
        return;
    }
    char outs[24] = "";
    size_t n      = 0;
    for (int o = 0; o < 8;) {
        if (!((mask >> o) & 1)) {
            ++o;
            continue;
        }
        int e = o;
        while (e + 1 < 8 && ((mask >> (e + 1)) & 1))
            ++e;
        n += static_cast<size_t>(snprintf(outs + n, sizeof(outs) - n, "%s%d", n ? "," : "", o + 1));
        if (e > o) n += static_cast<size_t>(snprintf(outs + n, sizeof(outs) - n, "-%d", e + 1));
        o = e + 1;
    }
    snprintf(out, cap, "%s (out %s)", base, outs);
}

static cJSON* ofl_channel(const config::ControlSlot& sl) {
    cJSON* ch     = cJSON_CreateObject();
    cJSON* caps   = cJSON_CreateArray();
    const auto fn = static_cast<config::CtlFn>(sl.fn);
    switch (fn) {
    case config::CtlFn::Master:
        cJSON_AddItemToObject(ch, "capability", ofl_cap("Intensity"));
        break;
    case config::CtlFn::Blackout: {
        ofl_range(caps, 0, 127, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Closed");
        ofl_range(caps, 128, 255, c);
        break;
    }
    case config::CtlFn::Strobe: {
        cJSON* c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Open");
        ofl_range(caps, 0, 0, c);
        c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Strobe");
        cJSON_AddStringToObject(c, "speedStart", "1Hz");
        cJSON_AddStringToObject(c, "speedEnd", "25Hz");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Scene: {
        ofl_range(caps, 0, 7, ofl_cap("NoFunction"));
        const size_t n = config::num_scenes();
        for (size_t i = 0; i < n; ++i) {
            cJSON* c = ofl_cap("Effect");
            cJSON_AddStringToObject(c, "effectName", config::get_scene(i).name);
            ofl_range(caps, static_cast<int>(8 * (i + 1)), static_cast<int>(8 * (i + 1) + 7), c);
        }
        if (8 * (n + 1) <= 255)
            ofl_range(caps, static_cast<int>(8 * (n + 1)), 255, ofl_cap("NoFunction"));
        break;
    }
    case config::CtlFn::Speed: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("EffectSpeed");
        cJSON_AddStringToObject(c, "speedStart", "slow");
        cJSON_AddStringToObject(c, "speedEnd", "fast");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Param: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("EffectParameter");
        cJSON_AddStringToObject(c, "parameterStart", "low");
        cJSON_AddStringToObject(c, "parameterEnd", "high");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Red:
    case config::CtlFn::Green:
    case config::CtlFn::Blue: {
        cJSON* c = ofl_cap("ColorIntensity");
        cJSON_AddStringToObject(c, "color",
                                fn == config::CtlFn::Red     ? "Red"
                                : fn == config::CtlFn::Green ? "Green"
                                                             : "Blue");
        cJSON_AddItemToObject(ch, "capability", c);
        break;
    }
    case config::CtlFn::Effect: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        int lo = 1;
        for (int v = 1; v <= 255; ++v) {
            const int e = dmx::effect_for_dmx_value(static_cast<uint8_t>(v));
            if (v == 255 || dmx::effect_for_dmx_value(static_cast<uint8_t>(v + 1)) != e) {
                cJSON* c = ofl_cap("Effect");
                cJSON_AddStringToObject(c, "effectName",
                                        config::scene_fx_label(static_cast<uint8_t>(e)));
                ofl_range(caps, lo, v, c);
                lo = v + 1;
            }
        }
        break;
    }
    case config::CtlFn::Fade: {
        cJSON* c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Scene fade time, value x 0.1 s");
        cJSON_AddItemToObject(ch, "capability", c);
        break;
    }
    case config::CtlFn::Fseq: {
        cJSON* c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Stop the show file");
        ofl_range(caps, 0, 7, c);
        c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Play show file n (8 values per file)");
        ofl_range(caps, 8, 255, c);
        break;
    }
    default: break;
    }
    if (cJSON_GetArraySize(caps) > 0)
        cJSON_AddItemToObject(ch, "capabilities", caps);
    else
        cJSON_Delete(caps);
    return ch;
}

static const char* ofl_base_name(const config::ControlSlot& sl, char* buf, size_t cap) {
    switch (static_cast<config::CtlFn>(sl.fn)) {
    case config::CtlFn::Master: return "Master";
    case config::CtlFn::Blackout: return "Blackout";
    case config::CtlFn::Strobe: return "Strobe";
    case config::CtlFn::Scene: return "Scene";
    case config::CtlFn::Speed: return "Scene speed";
    case config::CtlFn::Param: return "Scene parameter";
    case config::CtlFn::Red:
    case config::CtlFn::Green:
    case config::CtlFn::Blue:
        snprintf(buf, cap, "Colour %u %s", sl.index + 1u,
                 sl.fn == static_cast<uint8_t>(config::CtlFn::Red)     ? "red"
                 : sl.fn == static_cast<uint8_t>(config::CtlFn::Green) ? "green"
                                                                       : "blue");
        return buf;
    case config::CtlFn::Effect: return "Effect";
    case config::CtlFn::Fade: return "Fade time";
    case config::CtlFn::Fseq: return "Show file";
    default: return nullptr;
    }
}

// "Sep 30 2026" (app description) → "2026-09-30".
static void iso_date(char* out, size_t cap, const char* d) {
    static const char* const kMon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon                       = 1;
    for (int i = 0; i < 12; ++i)
        if (strncmp(d, kMon + 3 * i, 3) == 0) mon = i + 1;
    snprintf(out, cap, "%.4s-%02d-%02d", d + 7, mon, atoi(d + 4));
}

static cJSON* build_fixture_json() {
    const auto& c = config::get_control();
    cJSON* root   = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$schema",
                            "https://raw.githubusercontent.com/OpenLightingProject/"
                            "open-fixture-library/master/schemas/fixture.json");
    char name[48];
    snprintf(name, sizeof(name), "pixfrog %s control", config::get_global().short_name);
    cJSON_AddStringToObject(root, "name", name);
    cJSON* cats = cJSON_CreateArray();
    cJSON_AddItemToArray(cats, cJSON_CreateString("Other"));
    cJSON_AddItemToObject(root, "categories", cats);
    cJSON* meta    = cJSON_CreateObject();
    cJSON* authors = cJSON_CreateArray();
    cJSON_AddItemToArray(authors, cJSON_CreateString("pixfrog"));
    cJSON_AddItemToObject(meta, "authors", authors);
    char date[12];
    iso_date(date, sizeof(date), esp_app_get_description()->date);
    cJSON_AddStringToObject(meta, "createDate", date);
    cJSON_AddStringToObject(meta, "lastModifyDate", date);
    cJSON_AddItemToObject(root, "meta", meta);

    cJSON* avail    = cJSON_CreateObject();
    cJSON* mode_chs = cJSON_CreateArray();
    for (size_t i = 0; i < c.count; ++i) {
        const auto& sl = c.slots[i];
        char buf[32], base[64], unique[80];  // sized for GCC's worst case
        const char* b = ofl_base_name(sl, buf, sizeof(buf));
        if (!b) {  // spare channel
            cJSON_AddItemToArray(mode_chs, cJSON_CreateNull());
            continue;
        }
        ofl_channel_name(base, sizeof(base), b, sl.mask);
        snprintf(unique, sizeof(unique), "%s", base);
        for (int k = 2; cJSON_GetObjectItemCaseSensitive(avail, unique); ++k)
            snprintf(unique, sizeof(unique), "%s %d", base, k);
        cJSON* ch = ofl_channel(sl);
        cJSON_AddItemToArray(mode_chs, cJSON_CreateString(unique));
        if (config::control_slot_width(sl) == 2) {
            char fine[88];
            snprintf(fine, sizeof(fine), "%s fine", unique);
            cJSON* aliases = cJSON_CreateArray();
            cJSON_AddItemToArray(aliases, cJSON_CreateString(fine));
            cJSON_AddItemToObject(ch, "fineChannelAliases", aliases);
            cJSON_AddItemToArray(mode_chs, cJSON_CreateString(fine));
        }
        cJSON_AddItemToObject(avail, unique, ch);
    }
    cJSON_AddItemToObject(root, "availableChannels", avail);
    cJSON* modes = cJSON_CreateArray();
    cJSON* mode  = cJSON_CreateObject();
    char mname[24];
    snprintf(mname, sizeof(mname), "%u-channel",
             static_cast<unsigned>(config::control_footprint(c)));
    cJSON_AddStringToObject(mode, "name", mname);
    cJSON_AddItemToObject(mode, "channels", mode_chs);
    cJSON_AddItemToArray(modes, mode);
    cJSON_AddItemToObject(root, "modes", modes);
    return root;
}

static esp_err_t handle_control_fixture(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pixfrog-control.json\"");
    return send_json(req, build_fixture_json());
}

// ── POST /api/control ───────────────────────────────────────────────────────

static esp_err_t handle_post_control(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[2048];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    config::ControlConfig c = config::get_control();
    const char* why         = nullptr;
    const bool ok           = apply_control_json(j, c, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why);
    config::set_control(c);
    dmx::mark_global_dirty();  // maps (or drops) the control universe
    return send_json(req, build_control_json());
}

// ── POST /api/show — grand master, blackout, strobe (runtime) ───────────────
// {"outputs":255, "master":80, "blackout":true|false|"toggle", "strobe_hz":5}

static cJSON* build_show_json() {
    cJSON* js  = cJSON_CreateObject();
    cJSON* jm  = cJSON_CreateArray();
    cJSON* jl  = cJSON_CreateArray();
    cJSON* jst = cJSON_CreateArray();
    for (size_t o = 0; o < config::kNumChannels; ++o) {
        cJSON_AddItemToArray(
            jm,
            cJSON_CreateNumber((static_cast<unsigned>(dmx::master_effective(o)) * 1000u + 32767u) /
                               65535u / 10.0));
        cJSON_AddItemToArray(
            jl, cJSON_CreateNumber((static_cast<unsigned>(dmx::master_local(o)) * 1000u + 32767u) /
                                   65535u / 10.0));
        cJSON_AddItemToArray(jst, cJSON_CreateNumber(dmx::strobe_effective(o) / 10.0));
    }
    cJSON_AddItemToObject(js, "master", jm);
    cJSON_AddItemToObject(js, "master_local", jl);
    cJSON_AddNumberToObject(js, "blackout", dmx::blackout_effective());
    cJSON_AddNumberToObject(js, "blackout_local", dmx::blackout_local());
    cJSON_AddItemToObject(js, "strobe_hz", jst);
    cJSON_AddNumberToObject(js, "fade_ms", dmx::scene_fade_ms());
    cJSON_AddStringToObject(js, "control",
                            !config::get_control().enabled ? "off"
                            : dmx::control_live()          ? "live"
                                                           : "idle");
    cJSON* jso = cJSON_CreateArray();
    for (size_t o = 0; o < config::kNumChannels; ++o)
        cJSON_AddItemToArray(jso, cJSON_CreateNumber(dmx::scene_on_output(o)));
    cJSON_AddItemToObject(js, "scenes", jso);
    return js;
}

static esp_err_t handle_post_show(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[256];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    uint8_t outs    = dmx::kAllOutputs;
    const char* why = nullptr;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(j, "outputs");
    if (it && (!cJSON_IsNumber(it) || !(it->valuedouble >= 1 && it->valuedouble <= 255)))
        why = "outputs: 1..255";
    else if (it)
        outs = static_cast<uint8_t>(it->valuedouble);
    const cJSON* jm = cJSON_GetObjectItemCaseSensitive(j, "master");
    const cJSON* jb = cJSON_GetObjectItemCaseSensitive(j, "blackout");
    const cJSON* js = cJSON_GetObjectItemCaseSensitive(j, "strobe_hz");
    if (!why && jm && (!cJSON_IsNumber(jm) || !(jm->valuedouble >= 0 && jm->valuedouble <= 100)))
        why = "master: 0..100";
    if (!why && jb && !cJSON_IsBool(jb) &&
        !(cJSON_IsString(jb) && !strcmp(jb->valuestring, "toggle")))
        why = "blackout: true|false|\"toggle\"";
    if (!why && js && (!cJSON_IsNumber(js) || !(js->valuedouble >= 0 && js->valuedouble <= 25)))
        why = "strobe_hz: 0..25";
    if (why) {
        cJSON_Delete(j);
        return send_err(req, 400, why);
    }
    if (jm) dmx::master_set(outs, static_cast<uint16_t>(jm->valuedouble * 65535.0 / 100.0 + 0.5));
    if (jb) {
        if (cJSON_IsString(jb))
            dmx::blackout_toggle(outs);
        else
            dmx::blackout_set(outs, cJSON_IsTrue(jb));
    }
    if (js) dmx::strobe_set(outs, static_cast<uint8_t>(js->valuedouble * 10.0 + 0.5));
    cJSON_Delete(j);
    return send_json(req, build_show_json());
}

// ── GET /api/config ─────────────────────────────────────────────────────────

static cJSON* build_global_json() {
    const auto& g = config::get_global();
    cJSON* jg     = cJSON_CreateObject();
    cJSON_AddBoolToObject(jg, "dhcp", g.use_dhcp);
    cJSON_AddStringToObject(jg, "ip_fallback", config::ip_fallback_id(g.ip_fallback));

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
    cJSON_AddNumberToObject(jg, "tft_brightness", config::tft_brightness_pct(g));
    cJSON_AddNumberToObject(jg, "tft_idle_dim", config::tft_idle_dim_pct(g));
    cJSON_AddNumberToObject(jg, "tft_dim_delay_s", config::tft_dim_delay_s(g));
    cJSON_AddBoolToObject(jg, "web_enabled", g.web_enabled);
    cJSON_AddBoolToObject(jg, "sacn_enabled", g.sacn_enabled);
    cJSON_AddBoolToObject(jg, "fpp_remote", g.fpp_remote);
    cJSON_AddBoolToObject(jg, "auth_enabled", config::web_password_set());
    cJSON_AddNumberToObject(jg, "failsafe_mode", g.failsafe_mode);
    cJSON_AddNumberToObject(jg, "failsafe_timeout_s", g.failsafe_timeout_s);
    cJSON_AddNumberToObject(jg, "failsafe_scene", g.failsafe_scene);
    cJSON_AddNumberToObject(jg, "boot_scene", g.boot_scene);
    cJSON_AddNumberToObject(jg, "merge_mode", g.merge_mode);
    cJSON_AddNumberToObject(jg, "lang", g.language);
    cJSON_AddNumberToObject(jg, "scene_fade_ms", g.scene_fade_ms);
    char fscol[8];
    snprintf(fscol, sizeof(fscol), "#%02x%02x%02x", g.failsafe_r, g.failsafe_g, g.failsafe_b);
    cJSON_AddStringToObject(jg, "failsafe_color", fscol);
    return jg;
}

static cJSON* build_scenes_json() {
    cJSON* jscenes = cJSON_CreateArray();
    for (size_t i = 0; i < config::num_scenes(); ++i) {
        const auto& sc = config::get_scene(i);
        cJSON* js      = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", sc.name);
        cJSON_AddNumberToObject(js, "effect", sc.effect);
        char col[8];
        snprintf(col, sizeof(col), "#%02x%02x%02x", sc.r, sc.g, sc.b);
        cJSON_AddStringToObject(js, "color", col);
        cJSON* jcols = cJSON_AddArrayToObject(js, "colors");
        for (size_t k = 0; k < config::scene_num_colors(sc); ++k) {
            uint8_t rgb[3];
            config::scene_color(sc, k, rgb);
            snprintf(col, sizeof(col), "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
            cJSON_AddItemToArray(jcols, cJSON_CreateString(col));
        }
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
        cJSON_AddNumberToObject(jc, "max_pixels", dmx::channel_max_pixels(i));
        cJSON_AddNumberToObject(jc, "brightness", c.brightness);
        cJSON_AddNumberToObject(jc, "grouping", c.grouping);
        cJSON_AddBoolToObject(jc, "invert", c.invert_direction);
        cJSON_AddNumberToObject(jc, "clock_hz", static_cast<double>(c.clock_hz));
        cJSON_AddNumberToObject(jc, "gamma_x10", c.gamma_x10);
        char wb[8];
        snprintf(wb, sizeof(wb), "#%02x%02x%02x", c.wb_r, c.wb_g, c.wb_b);
        cJSON_AddStringToObject(jc, "wb", wb);
        // [[first dead LED, 1-based physical], count], ...
        cJSON* jg = cJSON_AddArrayToObject(jc, "gaps");
        for (size_t k = 0; k < led::gap_count(c.gaps, led::kMaxPixelGaps); ++k) {
            cJSON* pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(c.gaps[k].pos + 1));
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(c.gaps[k].len));
            cJSON_AddItemToArray(jg, pair);
        }
        cJSON_AddItemToArray(jchs, jc);
    }
    return jchs;
}

// "gaps": [[first dead LED (1-based), count], ...] replaces the whole list when
// present and well-formed (≤ kMaxPixelGaps pairs, in range); config_store
// normalizes (sort + merge) on write.
static void apply_gaps_json(const cJSON* jc, config::ChannelConfig& c) {
    const cJSON* jg = cJSON_GetObjectItemCaseSensitive(jc, "gaps");
    if (!cJSON_IsArray(jg) || cJSON_GetArraySize(jg) > static_cast<int>(led::kMaxPixelGaps)) return;
    led::PixelGap parsed[led::kMaxPixelGaps] = {};
    size_t n                                 = 0;
    for (const cJSON* pair = jg->child; pair; pair = pair->next) {
        const cJSON* jp = cJSON_GetArrayItem(pair, 0);
        const cJSON* jl = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jl) || jp->valuedouble < 1 ||
            jp->valuedouble > led::kMaxPixelsPerChannel || jl->valuedouble < 0 ||
            jl->valuedouble > led::kMaxPixelsPerChannel)
            return;
        parsed[n].pos   = static_cast<uint16_t>(jp->valuedouble - 1);
        parsed[n++].len = static_cast<uint16_t>(jl->valuedouble);
    }
    std::memcpy(c.gaps, parsed, sizeof(c.gaps));
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
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
    return send_json(req, root);
}

// ── GET /api/status ─────────────────────────────────────────────────────────
// Lightweight live status for SPA polling: no config blobs, just the values
// that change at runtime. Unauthenticated like the other GETs.

static cJSON* build_status_json() {
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

static esp_err_t handle_get_status(httpd_req_t* req) {
    return send_json(req, build_status_json());
}

// ── GET /api/ws — live status pushed over a WebSocket ───────────────────────
// The SPA opens one and stops polling /api/status: web_push_task builds the
// same JSON once a second ({"type":"status", …}) plus a 5 Hz binary preview
// of every output (push_preview), and the httpd task sends them to every
// WebSocket client (httpd_queue_work: sends happen in the server's
// own task). Read-only and unauthenticated, like the GET it replaces.

namespace {
std::atomic<uint32_t> g_ws_seen{ 0 };      // a client connected since start (32-bit: P4 RMW)
std::atomic<uint32_t> g_push_run{ 0 };     // web_push_task keeps going while 1
SemaphoreHandle_t g_server_mux = nullptr;  // g_server vs the push task's use of it
constexpr size_t kMaxWsClients = 8;
}  // namespace

static esp_err_t handle_ws(httpd_req_t* req) {
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
static void web_push_task(void*) {
    uint32_t tick = 0;
    while (g_push_run.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!g_ws_seen.load(std::memory_order_relaxed)) continue;  // nobody listening yet
        push_preview();
        if (++tick % 5 == 0) push_status();
    }
    vTaskDelete(nullptr);
}

// ── GET /api/diag — "stats for nerds" ───────────────────────────────────────
// Deeper telemetry than /api/status: render-pipeline timings, frame counters,
// memory (incl. PSRAM + frame-buffer footprint), system identity and the
// reset reason, plus per-channel capacity. Polled only while the Diagnostics
// tab is open.

static esp_err_t handle_get_diag(httpd_req_t* req) {
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

static esp_err_t handle_get_logs(httpd_req_t* req) {
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

static esp_err_t handle_post_loglevel(httpd_req_t* req) {
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

static esp_err_t handle_coredump_get(httpd_req_t* req) {
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

static esp_err_t handle_coredump_delete(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (esp_core_dump_image_erase() != ESP_OK) return send_err(req, 500, "erase failed");
    return send_ok(req);
}

// ── FSEQ playlist JSON (GET/POST /api/fseq/playlist, config, backup) ───────

static const char* fseq_name_error(const char* name);

static cJSON* build_playlist_json() {
    const config::FseqPlaylist& p = config::get_playlist();
    cJSON* root                   = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "loop", p.loop);
    cJSON_AddBoolToObject(root, "autostart", p.autostart);
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < p.count; ++i) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", p.items[i].name);
        cJSON_AddNumberToObject(it, "repeat", p.items[i].repeat);
        cJSON_AddItemToArray(items, it);
    }
    return root;
}

// Applies `j` onto `p` (fields absent keep their value; `items` replaces the
// whole list). False with *why on the first invalid field, `p` then partial.
static bool apply_playlist_json(const cJSON* j, config::FseqPlaylist& p, const char** why) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(j, "loop");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "loop: true|false", false;
        p.loop = cJSON_IsTrue(it) ? 1 : 0;
    }
    it = cJSON_GetObjectItemCaseSensitive(j, "autostart");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "autostart: true|false", false;
        p.autostart = cJSON_IsTrue(it) ? 1 : 0;
    }
    const cJSON* items = cJSON_GetObjectItemCaseSensitive(j, "items");
    if (!items) return true;
    if (!cJSON_IsArray(items)) return *why = "items: array", false;
    if (cJSON_GetArraySize(items) > static_cast<int>(config::kPlaylistMax))
        return *why = "items: 16 at most", false;
    config::FseqPlaylist out = p;
    out.count                = 0;
    for (const cJSON* e = items->child; e; e = e->next) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(e, "name");
        if (!cJSON_IsString(name) || !name->valuestring)
            return *why = "items[].name: string", false;
        if (const char* bad = fseq_name_error(name->valuestring)) return *why = bad, false;
        uint8_t repeat   = 1;
        const cJSON* rep = cJSON_GetObjectItemCaseSensitive(e, "repeat");
        if (rep) {
            if (!cJSON_IsNumber(rep) || !(rep->valuedouble >= 1 && rep->valuedouble <= 255))
                return *why = "items[].repeat: 1..255", false;
            repeat = static_cast<uint8_t>(rep->valuedouble);
        }
        config::PlaylistItem& dst = out.items[out.count++];
        dst                       = config::PlaylistItem{};
        std::strncpy(dst.name, name->valuestring, sizeof(dst.name) - 1);
        dst.repeat = repeat;
    }
    for (size_t i = out.count; i < config::kPlaylistMax; ++i)
        out.items[i] = config::PlaylistItem{};
    p = out;
    return true;
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
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
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
    it = cJSON_GetObjectItemCaseSensitive(jg, "ip_fallback");
    if (cJSON_IsString(it) && config::ip_fallback_from_id(it->valuestring) >= 0)
        g.ip_fallback = static_cast<uint8_t>(config::ip_fallback_from_id(it->valuestring));
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
    if (num("refresh_hz", config::kMinRefreshHz, config::kMaxRefreshHz, &v))
        g.refresh_rate_hz = static_cast<uint8_t>(v);
    if (num("home_timeout_s", 0, 65535, &v)) g.home_timeout_s = static_cast<uint16_t>(v);
    if (num("tft_brightness", config::kTftBrightnessMin, 100, &v))
        g.tft_brightness = static_cast<uint8_t>(v);
    if (num("tft_idle_dim", 0, 100, &v)) g.tft_idle_dim = static_cast<uint8_t>(v);
    if (num("tft_dim_delay_s", 0, config::kTftDimDelayMaxS, &v))
        g.tft_dim_delay_s = static_cast<uint16_t>(v);
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
    // Bounded by the list capacity, not its current length: a reference past
    // the end plays nothing (scene_start / get_scene bound-check at use).
    if (num("failsafe_scene", 0, config::kMaxScenes - 1, &v))
        g.failsafe_scene = static_cast<uint8_t>(v);
    if (num("boot_scene", 0, config::kMaxScenes, &v)) g.boot_scene = static_cast<uint8_t>(v);
    if (num("merge_mode", 0, 1, &v)) g.merge_mode = static_cast<uint8_t>(v);
    if (getb("fpp_remote", &bv)) g.fpp_remote = bv;
    if (num("lang", 0, 1, &v)) g.language = static_cast<uint8_t>(v);
    if (num("scene_fade_ms", 0, config::kMaxSceneFadeMs, &v))
        g.scene_fade_ms = static_cast<uint16_t>(v);
    config::set_global(g);
}

static void restore_channel(size_t i, cJSON* jc) {
    auto c    = config::get_channel(i);
    cJSON* it = cJSON_GetObjectItemCaseSensitive(jc, "protocol");
    if (cJSON_IsString(it)) {
        const int pv = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT),
                              it->valuestring);
        if (pv >= 0) c.protocol = static_cast<led::Protocol>(pv);
        // A backup from before DMX512 output moved to the DMX node firmware:
        // that channel described a DMX universe, not a strip — disable it.
        if (std::strcmp(it->valuestring, "DMX512") == 0) c.protocol = led::Protocol::Off;
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
    if (num("universe_start", 0, 32767, &v)) c.universe_start = static_cast<uint16_t>(v);
    if (num("dmx_start", 1, 512, &v)) c.dmx_start = static_cast<uint16_t>(v);
    if (num("pixel_count", 1, dmx::kMaxPixelsPerChan, &v)) c.pixel_count = static_cast<uint16_t>(v);
    if (num("brightness", 0, 255, &v)) c.brightness = static_cast<uint8_t>(v);
    if (num("grouping", 1, 8, &v)) c.grouping = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jc, "invert");
    if (cJSON_IsBool(it)) c.invert_direction = cJSON_IsTrue(it);
    if (num("clock_hz", led::kMinClockHz, led::kMaxClockHz, &v))
        c.clock_hz = static_cast<uint32_t>(v);
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
    apply_gaps_json(jc, c);
    config::set_channel(i, c);
    dmx::mark_channel_dirty(i);
}

static bool parse_hex_color(const cJSON* it, uint8_t rgb[3]) {
    if (!cJSON_IsString(it)) return false;
    const char* cs = it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring;
    unsigned r, g, b;
    if (sscanf(cs, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    rgb[0] = static_cast<uint8_t>(r);
    rgb[1] = static_cast<uint8_t>(g);
    rgb[2] = static_cast<uint8_t>(b);
    return true;
}

// Partial update: absent or out-of-range fields keep their current value.
// "colors" (1..kSceneColorsMax) wins over the single legacy "color".
static void apply_scene_json(const cJSON* js, config::Scene& sc) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(js, "name");
    if (cJSON_IsString(it)) {
        memset(sc.name, 0, sizeof(sc.name));
        strncpy(sc.name, it->valuestring, sizeof(sc.name) - 1);
    }
    auto num = [&](const char* k, double hi, uint8_t* out) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(js, k);
        if (cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble <= hi)
            *out = static_cast<uint8_t>(n->valuedouble);
    };
    num("effect", config::kSceneFxCount - 1, &sc.effect);
    num("speed", 255, &sc.speed);
    num("param", 255, &sc.param);
    num("mask", 255, &sc.channel_mask);

    uint8_t rgb[3];
    const cJSON* cols = cJSON_GetObjectItemCaseSensitive(js, "colors");
    const int ncols   = cJSON_IsArray(cols) ? cJSON_GetArraySize(cols) : 0;
    if (ncols >= 1 && static_cast<size_t>(ncols) <= config::kSceneColorsMax) {
        uint8_t parsed[config::kSceneColorsMax][3];
        bool ok = true;
        for (int k = 0; k < ncols && ok; ++k)
            ok = parse_hex_color(cJSON_GetArrayItem(cols, k), parsed[k]);
        if (ok) {
            for (int k = 0; k < ncols; ++k)
                config::set_scene_color(sc, static_cast<size_t>(k), parsed[k][0], parsed[k][1],
                                        parsed[k][2]);
            sc.num_colors = static_cast<uint8_t>(ncols);
        }
    } else if (parse_hex_color(cJSON_GetObjectItemCaseSensitive(js, "color"), rgb)) {
        config::set_scene_color(sc, 0, rgb[0], rgb[1], rgb[2]);
    }
}

// The backup's list replaces the current one wholesale (length included).
static void restore_scenes(const cJSON* jsc) {
    static config::Scene list[config::kMaxScenes];  // httpd stack is small
    size_t n = 0;
    for (const cJSON* js = jsc->child; js && n < config::kMaxScenes; js = js->next) {
        list[n]              = config::Scene{};
        list[n].channel_mask = 0xFF;
        list[n].num_colors   = 1;
        apply_scene_json(js, list[n++]);
    }
    dmx::scene_stop();  // the playing index may point elsewhere now
    config::replace_scenes(list, n);
}

static esp_err_t handle_restore(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // Full backup ≈ 3 kB + ~170 B per scene (30 max); static keeps it off the httpd stack.
    static char buf[12288];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    cJSON* jsc = cJSON_GetObjectItemCaseSensitive(j, "scenes");
    if (cJSON_IsArray(jsc)) restore_scenes(jsc);
    const config::GlobalConfig before = config::get_global();
    cJSON* jg                         = cJSON_GetObjectItemCaseSensitive(j, "global");
    if (cJSON_IsObject(jg)) restore_global(jg);
    // Control mode: applied only when the whole object is valid.
    cJSON* jctl = cJSON_GetObjectItemCaseSensitive(j, "control");
    if (cJSON_IsObject(jctl)) {
        config::ControlConfig c = config::get_control();
        const char* why         = nullptr;
        if (apply_control_json(jctl, c, &why)) config::set_control(c);
    }
    // Playlist: likewise all or nothing.
    cJSON* jpl = cJSON_GetObjectItemCaseSensitive(j, "playlist");
    if (cJSON_IsObject(jpl)) {
        config::FseqPlaylist p = config::get_playlist();
        const char* why        = nullptr;
        if (apply_playlist_json(jpl, p, &why)) config::set_playlist(p);
    }
    cJSON* jchs = cJSON_GetObjectItemCaseSensitive(j, "channels");
    if (cJSON_IsArray(jchs)) {
        const int n = cJSON_GetArraySize(jchs);
        for (int i = 0; i < n && i < static_cast<int>(config::kNumChannels); ++i)
            restore_channel(static_cast<size_t>(i), cJSON_GetArrayItem(jchs, i));
    }
    cJSON_Delete(j);
    dmx::mark_global_dirty();

    // Same live side effects as POST /api/global: the receivers follow their
    // flags now, not at the next reboot.
    const config::GlobalConfig& g = config::get_global();
    if (g.sacn_enabled != before.sacn_enabled) {
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
    }
    if (g.fpp_remote != before.fpp_remote) {
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":true,\"note\":\"network/web changes apply after reboot\"}");
}

// A handler cannot stop the server it runs in (httpd_stop waits for it):
// answer first, then stop from a short-lived task.
static void web_stop_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(300));
    stop();
    vTaskDelete(nullptr);
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
        // Range-check the double first: casting an out-of-range value
        // (-5, 1e40) to uint32_t is undefined (RISC-V saturates -5 to 0).
        const double d = item->valuedouble;
        if (!(d >= lo && d <= hi)) return false;
        out = static_cast<uint32_t>(d);
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
    // Read when the fallback is taken: no reboot needed.
    if ((s = get_str("ip_fallback"))) {
        const int fb = config::ip_fallback_from_id(s);
        if (fb < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "ip_fallback: linklocal|artnet");
        }
        g.ip_fallback = static_cast<uint8_t>(fb);
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
    if (get_u32("refresh_hz", config::kMinRefreshHz, config::kMaxRefreshHz, u))
        g.refresh_rate_hz = static_cast<uint8_t>(u);
    if (get_u32("home_timeout_s", 0, 65535, u)) g.home_timeout_s = static_cast<uint16_t>(u);
    if (get_u32("tft_brightness", config::kTftBrightnessMin, 100, u))
        g.tft_brightness = static_cast<uint8_t>(u);
    if (get_u32("tft_idle_dim", 0, 100, u)) g.tft_idle_dim = static_cast<uint8_t>(u);
    if (get_u32("tft_dim_delay_s", 0, config::kTftDimDelayMaxS, u))
        g.tft_dim_delay_s = static_cast<uint16_t>(u);
    bool web_off = false;  // this request turns the web UI off
    if (get_bool("web_enabled", b)) {
        g.web_enabled = b;
        web_off       = !b;
    }
    if (get_u32("failsafe_mode", 0, 3, u)) g.failsafe_mode = static_cast<uint8_t>(u);
    if (get_u32("failsafe_scene", 0, config::kMaxScenes - 1, u))
        g.failsafe_scene = static_cast<uint8_t>(u);
    if (get_u32("boot_scene", 0, config::kMaxScenes, u)) g.boot_scene = static_cast<uint8_t>(u);
    if (get_u32("failsafe_timeout_s", 0, 3600, u)) g.failsafe_timeout_s = static_cast<uint16_t>(u);
    if (get_u32("merge_mode", 0, 1, u)) g.merge_mode = static_cast<uint8_t>(u);
    if (get_u32("lang", 0, 1, u)) g.language = static_cast<uint8_t>(u);
    if (get_u32("scene_fade_ms", 0, config::kMaxSceneFadeMs, u))
        g.scene_fade_ms = static_cast<uint16_t>(u);
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
    // backing buffer. Never echoed back. Too long is refused, not truncated:
    // a truncated hash would never match what the browser sends back.
    char pwd[config::kMaxWebPasswordLen + 1];
    bool password_changed = false;
    if ((s = get_str("web_password"))) {
        if (strlen(s) > config::kMaxWebPasswordLen) {
            cJSON_Delete(j);
            return send_err(req, 400, "web_password: at most 63 characters");
        }
        strcpy(pwd, s);
        password_changed = true;
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

    // Turning the web UI off from the web UI: this server stops right after
    // the answer (it used to keep serving until a reboot).
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed)
        cJSON_AddStringToObject(resp, "note", "network_changes_apply_after_reboot");
    if (web_off) cJSON_AddBoolToObject(resp, "web_stopping", true);
    const esp_err_t r = send_json(req, resp);
    if (web_off && xTaskCreate(web_stop_task, "web_stop", 3072, nullptr, 5, nullptr) != pdPASS)
        ESP_LOGE(TAG, "web_stop task create failed: the server runs until reboot");
    return r;
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
        // Range-check the double first: casting an out-of-range value
        // (-5, 1e40) to uint32_t is undefined (RISC-V saturates -5 to 0).
        const double d = item->valuedouble;
        if (!(d >= lo && d <= hi)) return false;
        out = static_cast<uint32_t>(d);
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
    if (get_u32("universe_start", 0, 32767, u)) c.universe_start = static_cast<uint16_t>(u);
    if (get_u32("dmx_start", 1, 512, u)) c.dmx_start = static_cast<uint16_t>(u);
    if (get_u32("pixel_count", 1, dmx::kMaxPixelsPerChan, u))
        c.pixel_count = static_cast<uint16_t>(u);
    if (get_u32("brightness", 0, 255, u)) c.brightness = static_cast<uint8_t>(u);
    if (get_u32("grouping", 1, 8, u)) c.grouping = static_cast<uint8_t>(u);
    if (get_bool("invert", b)) c.invert_direction = b;
    if (get_u32("clock_hz", led::kMinClockHz, led::kMaxClockHz, u)) c.clock_hz = u;
    if (get_u32("gamma_x10", 10, 40, u)) c.gamma_x10 = static_cast<uint8_t>(u);
    if ((s = get_str("wb"))) {
        unsigned wr, wg, wbv;
        if (sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &wr, &wg, &wbv) == 3) {
            c.wb_r = wr ? static_cast<uint8_t>(wr) : 255;
            c.wb_g = wg ? static_cast<uint8_t>(wg) : 255;
            c.wb_b = wbv ? static_cast<uint8_t>(wbv) : 255;
        }
    }
    apply_gaps_json(j, c);

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

// Exchanged, not just read: one flight at a time, whatever the httpd task
// count is configured to be.
static std::atomic<uint32_t> g_ota_in_progress{ 0 };  // RMW: 32-bit on the P4

static esp_err_t handle_ota(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (g_ota_in_progress.exchange(1)) return send_err(req, 500, "OTA already in progress");

    // Claimed above — every exit from here on must hand the flag back.
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) {
        g_ota_in_progress = false;
        return send_err(req, 500, "no OTA partition (single-app table?)");
    }

    const int total = req->content_len;
    if (total <= 0) {
        g_ota_in_progress = false;
        return send_err(req, 400, "empty body");
    }
    if (static_cast<size_t>(total) > update->size) {
        g_ota_in_progress = false;
        return send_err(req, 400, "image too large");
    }

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

    // The new image overwrote the rejected slot: forget which image that was,
    // so a rollback of this upload is recorded as a new event even when it is
    // the same build (boot matches the record by image SHA).
    config::RollbackRecord rb{};
    if (config::get_rollback(rb)) {
        std::memset(rb.rejected_sha, 0, sizeof(rb.rejected_sha));
        config::set_rollback(rb);
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

// ── Scenes ───────────────────────────────────────────────────────────────────
// POST /api/scene/{n}          partial update (apply_scene_json)
// POST /api/scene/{n}/play     play it
// POST /api/scene/{n}/delete   remove it; later scenes shift down by one
// POST /api/scenes/add         append (optional scene JSON body) → {"index":n}
// POST /api/scenes/move        {"from":a,"to":b}
// POST /api/scenes/stop

static esp_err_t handle_post_scene(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;

    const char* tail = req->uri + strlen("/api/scene/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::num_scenes())
        return send_err(req, 404, "no such scene");

    if (strstr(tail, "/play")) {
        // Optional {"outputs": mask}: the zone to claim (∩ the scene's mask).
        uint8_t outs = dmx::kAllOutputs;
        if (req->content_len > 0) {
            char pb[64];
            if (!read_body(req, pb, sizeof(pb) - 1)) return send_err(req, 400, "body too large");
            cJSON* pj       = cJSON_Parse(pb);
            const cJSON* jo = pj ? cJSON_GetObjectItemCaseSensitive(pj, "outputs") : nullptr;
            const bool bad  = !pj || (jo && (!cJSON_IsNumber(jo) ||
                                            !(jo->valuedouble >= 1 && jo->valuedouble <= 255)));
            if (!bad && jo) outs = static_cast<uint8_t>(jo->valuedouble);
            cJSON_Delete(pj);
            if (bad) return send_err(req, 400, "outputs: 1..255");
        }
        dmx::scene_start_on(static_cast<uint8_t>(idx), outs);
        return send_ok(req);
    }
    if (strstr(tail, "/stop")) {
        dmx::scene_stop_scene(static_cast<uint8_t>(idx));
        return send_ok(req);
    }
    if (strstr(tail, "/delete")) {
        config::delete_scene(static_cast<size_t>(idx));
        dmx::scene_list_edited(config::SceneEdit::Delete, static_cast<size_t>(idx));
        return send_ok(req);
    }

    char buf[384];  // name + 4 colours + numbers
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    auto sc = config::get_scene(static_cast<size_t>(idx));
    apply_scene_json(j, sc);
    cJSON_Delete(j);
    config::set_scene(static_cast<size_t>(idx), sc);
    return send_ok(req);
}

static esp_err_t handle_scenes_add(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    config::Scene sc{};
    std::strncpy(sc.name, "New scene", sizeof(sc.name) - 1);
    sc.channel_mask = 0xFF;
    sc.num_colors   = 1;
    sc.r = sc.g = sc.b = 255;
    if (req->content_len > 0) {
        char buf[384];
        if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large");
        cJSON* j = cJSON_Parse(buf);
        if (!j) return send_err(req, 400, "invalid JSON");
        apply_scene_json(j, sc);
        cJSON_Delete(j);
    }
    const int idx = config::add_scene(sc);
    if (idx < 0) return send_err(req, 409, "scene list full");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "index", idx);
    return send_json(req, root);
}

static esp_err_t handle_scenes_move(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[64];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    const cJSON* jf = cJSON_GetObjectItemCaseSensitive(j, "from");
    const cJSON* jt = cJSON_GetObjectItemCaseSensitive(j, "to");
    const int n     = static_cast<int>(config::num_scenes());
    const bool ok   = cJSON_IsNumber(jf) && cJSON_IsNumber(jt) && jf->valueint >= 0 &&
                    jf->valueint < n && jt->valueint >= 0 && jt->valueint < n;
    const auto from = ok ? static_cast<size_t>(jf->valueint) : 0;
    const auto to   = ok ? static_cast<size_t>(jt->valueint) : 0;
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, "from/to: existing scene indices");
    config::move_scene(from, to);
    dmx::scene_list_edited(config::SceneEdit::Move, from, to);
    return send_ok(req);
}

// ── POST /api/rollback/ack ───────────────────────────────────────────────────
static esp_err_t handle_rollback_ack(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (const auto* rb = rollback_record(); rb && !rb->acknowledged) {
        g_rollback.acknowledged = 1;
        config::set_rollback(g_rollback);
    }
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

// A file name the card may be asked for: a plain `name.fseq` in the mount
// root — no path separator, no hidden/relative name, no silent truncation.
// nullptr when acceptable, else the reason.
static const char* fseq_name_error(const char* name) {
    if (!name[0]) return "missing filename";
    if (strchr(name, '/') || strchr(name, '\\') || name[0] == '.') return "bad filename";
    const size_t nl = strlen(name);
    if (nl >= fseq::kMaxNameLen) return "filename too long";
    if (nl < 6 || strcasecmp(name + nl - 5, ".fseq") != 0) return "filename must end in .fseq";
    return nullptr;
}

static esp_err_t handle_fseq_play(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char body[fseq::kMaxNameLen + 64];
    if (!read_body(req, body, sizeof(body) - 1))
        return send_err(req, 400, "body too large or empty");
    cJSON* root = cJSON_Parse(body);
    if (!root) return send_err(req, 400, "Invalid JSON");
    // {"playlist":true} plays the stored playlist instead of one file.
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playlist"))) {
        cJSON_Delete(root);
        if (!fseq::start_playlist()) return send_err(req, 409, fseq::error_string());
        return send_ok(req);
    }
    const cJSON* jloop = cJSON_GetObjectItemCaseSensitive(root, "loop");
    if (jloop && !cJSON_IsBool(jloop)) {
        cJSON_Delete(root);
        return send_err(req, 400, "loop: true|false");
    }
    const bool loop = cJSON_IsTrue(jloop);
    cJSON* fn       = cJSON_GetObjectItemCaseSensitive(root, "filename");
    if (!cJSON_IsString(fn) || !fn->valuestring) {
        cJSON_Delete(root);
        return send_err(req, 400, "missing filename");
    }
    const char* why = fseq_name_error(fn->valuestring);
    if (why) {
        cJSON_Delete(root);
        return send_err(req, 400, why);
    }
    char filename[fseq::kMaxNameLen];
    strncpy(filename, fn->valuestring, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    cJSON_Delete(root);
    if (!fseq::start(filename, loop)) return send_err(req, 500, fseq::error_string());
    return send_ok(req);
}

// ── GET|POST /api/fseq/playlist ──────────────────────────────────────────────
// POST takes {loop?, autostart?, items?:[{name, repeat?}]}; items replaces the
// list. The playing playlist keeps its snapshot until restarted.

static esp_err_t handle_get_playlist(httpd_req_t* req) {
    return send_json(req, build_playlist_json());
}

static esp_err_t handle_post_playlist(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    static char buf[4096];  // 16 items × (64-byte name + JSON) — off the httpd stack
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    config::FseqPlaylist p = config::get_playlist();
    const char* why        = nullptr;
    const bool ok          = cJSON_IsObject(j) && apply_playlist_json(j, p, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why ? why : "expected an object");
    config::set_playlist(p);
    return send_json(req, build_playlist_json());
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

static std::atomic<uint32_t> g_fseq_upload_in_progress{ 0 };  // RMW: 32-bit

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
    if (g_fseq_upload_in_progress.exchange(1))
        return send_err(req, 500, "upload already in progress");
    // Claimed above — every exit from here on must hand the flag back.
    const auto reject = [req](int code, const char* msg) {
        g_fseq_upload_in_progress = false;
        return send_err(req, code, msg);
    };

    if (fseq::sd_state() != fseq::SdState::Mounted) return reject(500, "no SD card");

    char query[fseq::kMaxNameLen * 3 + 16] = {};
    char name[fseq::kMaxNameLen]           = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !name[0])
        return reject(400, "missing ?name=<file.fseq>");
    url_decode(name);

    if (const char* why = fseq_name_error(name)) return reject(400, why);

    const int total = req->content_len;
    if (total <= 0) return reject(400, "empty body");

    // Overwriting the file currently playing: stop playback to release it.
    const char* active = fseq::active_file();
    if (active && strcasecmp(active, name) == 0) fseq::stop();

    char tmp_path[96];
    char path[96 + fseq::kMaxNameLen];
    snprintf(tmp_path, sizeof(tmp_path), "%s/upload.part", fseq::kMountPath);
    snprintf(path, sizeof(path), "%s/%s", fseq::kMountPath, name);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return reject(500, "cannot create file on SD");
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

// Factory reset then reboot (what the SPA announces): the defaults turn the
// opt-in services (web, sACN, FPP, control universe) off, and only a reboot
// stops them all, the server answering this request included.
static esp_err_t handle_factory_reset(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    config::reset_to_defaults();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", true);
    send_json(req, resp);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
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
    const auto& g = config::get_global();
    mdns_instance_name_set(g.short_name);
    // TXT record lets the aggregated UI tell pixfrogs apart from other
    // _http._tcp hosts on the LAN, and carries the human node name + firmware.
    mdns_txt_item_t txt[3] = {
        { "product", "pixfrog" },
        { "node", g.short_name },
        { "fw", esp_app_get_description()->version },
    };
    mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 3);
    g_mdns_up = true;
    ESP_LOGI(TAG, "mDNS: pixfrog.local");
}

static void stop_mdns() {
    if (!g_mdns_up) return;
    mdns_free();
    g_mdns_up = false;
}

// ── POST /api/autopatch ─────────────────────────────────────────────────────
// Re-address every channel contiguously from a base universe (cascade by each
// channel's pixel span). Body: {"base": <0..32767>}. Replies the next free
// universe past the last channel.
static esp_err_t handle_autopatch(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[64];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    cJSON* base_item = cJSON_GetObjectItemCaseSensitive(j, "base");
    const bool ok    = base_item && cJSON_IsNumber(base_item) && base_item->valuedouble >= 0 &&
                    base_item->valuedouble <= 32767;
    const uint16_t base = ok ? static_cast<uint16_t>(base_item->valuedouble) : 0;
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, "base 0..32767");

    uint16_t next = 0;
    dmx::auto_patch_universes(base, &next);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "next_free", next);
    return send_json(req, root);
}

// ── GET /api/peers ──────────────────────────────────────────────────────────
// Browse the LAN for other pixfrogs (mDNS PTR on _http._tcp, filtered by the
// product=pixfrog TXT record) so the SPA can build the multi-node UI. Self is
// always listed first with self:true. The mDNS browse blocks, so the JSON is
// cached briefly to keep the SPA's periodic poll from stalling the server.

static const char* txt_value(const mdns_result_t* r, const char* key) {
    for (size_t i = 0; i < r->txt_count; ++i)
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) return r->txt[i].value;
    return nullptr;
}

static void build_peers_json(char* out, size_t cap) {
    char self_ip[16]   = "";
    const uint32_t cur = ui::get_ip();
    if (cur) fmt_ip(self_ip, sizeof(self_ip), cur);
    const auto& g = config::get_global();

    char seen[16][16];
    size_t nseen = 0;
    snprintf(seen[nseen++], sizeof(seen[0]), "%s", self_ip);

    cJSON* arr = cJSON_CreateArray();
    cJSON* me  = cJSON_CreateObject();
    cJSON_AddStringToObject(me, "name", g.short_name);
    cJSON_AddStringToObject(me, "ip", self_ip);
    cJSON_AddNumberToObject(me, "port", 80);
    cJSON_AddStringToObject(me, "fw", esp_app_get_description()->version);
    cJSON_AddBoolToObject(me, "self", true);
    cJSON_AddItemToArray(arr, me);

    mdns_result_t* res = nullptr;
    if (mdns_query_ptr("_http", "_tcp", 750, 16, &res) == ESP_OK) {
        for (mdns_result_t* r = res; r; r = r->next) {
            const char* product = txt_value(r, "product");
            if (!product || strcmp(product, "pixfrog") != 0) continue;

            char ip[16] = "";
            for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a->addr.u_addr.ip4.addr);
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                    break;
                }
            }
            if (ip[0] == '\0') continue;
            bool dup = false;
            for (size_t i = 0; i < nseen; ++i)
                if (strcmp(seen[i], ip) == 0) {
                    dup = true;
                    break;
                }
            if (dup || nseen >= 16) continue;
            snprintf(seen[nseen++], sizeof(seen[0]), "%s", ip);

            const char* node = txt_value(r, "node");
            const char* fw   = txt_value(r, "fw");
            cJSON* o         = cJSON_CreateObject();
            cJSON_AddStringToObject(
                o, "name", node ? node : (r->instance_name ? r->instance_name : "pixfrog"));
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddNumberToObject(o, "port", r->port ? r->port : 80);
            cJSON_AddStringToObject(o, "fw", fw ? fw : "");
            cJSON_AddBoolToObject(o, "self", false);
            cJSON_AddItemToArray(arr, o);
        }
        mdns_query_results_free(res);
    }

    char* str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    snprintf(out, cap, "%s", str ? str : "[]");
    if (str) cJSON_free(str);
}

static esp_err_t handle_get_peers(httpd_req_t* req) {
    static char cache[2048];
    static int64_t cache_us = 0;
    const int64_t now       = esp_timer_get_time();
    if (cache[0] == '\0' || now - cache_us > 5'000'000) {
        build_peers_json(cache, sizeof(cache));
        cache_us = now;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, cache);
}

}  // namespace

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
    cfg.max_uri_handlers = 40;  // 32 routes today
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
