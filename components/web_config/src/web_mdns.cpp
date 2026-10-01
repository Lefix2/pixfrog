// mDNS: the unique name, the elected pixfrog.local alias, /api/peers.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── mDNS ────────────────────────────────────────────────────────────────────
// Advertised only while the web UI is enabled — mDNS rides the web_enabled
// opt-in, no extra flag. Every box is pixfrog-<mac4>.local; the shared alias
// pixfrog.local is held by one box at a time (hub_election.h): web_hub_task
// browses the siblings every kBrowseMs and adds or drops the alias as a
// delegated hostname pointing at this box. Instance name = the ArtNet short
// name; the TXT record carries product/node/fw plus mac and hub (preferred)
// so the siblings can run the same election.

uint8_t g_mac[6]{};
char g_hostname[hub::kHostnameMax] = "pixfrog";
bool g_mdns_up                     = false;
std::atomic<uint32_t> g_hub_run{ 0 };    // web_hub_task keeps going while 1
std::atomic<uint32_t> g_hub_alive{ 0 };  // 1 until web_hub_task has left
std::atomic<uint32_t> g_alias_held{ 0 };
std::atomic<uint32_t> g_siblings{ 0 };  // live siblings in the roster
hub::Roster g_roster;                   // web_hub_task only

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

hub::Candidate self_candidate() {
    hub::Candidate c{};
    std::memcpy(c.mac, g_mac, 6);
    c.preferred = config::get_global().hub_preferred != 0;
    return c;
}

// ui::get_ip() is host order; lwIP keeps IPv4 in network order.
mdns_ip_addr_t self_addr(uint32_t ip) {
    mdns_ip_addr_t a{};
    a.addr.type            = ESP_IPADDR_TYPE_V4;
    a.addr.u_addr.ip4.addr = ((ip >> 24) & 0xFFu) | ((ip >> 8) & 0xFF00u) |
                             ((ip << 8) & 0xFF0000u) | ((ip << 24) & 0xFF000000u);
    a.next = nullptr;
    return a;
}

const char* txt_value(const mdns_result_t* r, const char* key) {
    for (size_t i = 0; i < r->txt_count; ++i)
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) return r->txt[i].value;
    return nullptr;
}

// One browse: every pixfrog sibling that advertises its MAC joins the roster
// (an older firmware without one cannot take part in the election).
void browse_siblings() {
    mdns_result_t* res = nullptr;
    if (mdns_query_ptr("_http", "_tcp", 750, hub::kMaxPeers, &res) != ESP_OK) return;
    const uint32_t now = now_ms();
    for (mdns_result_t* r = res; r; r = r->next) {
        const char* product = txt_value(r, "product");
        if (!product || strcmp(product, "pixfrog") != 0) continue;
        hub::Candidate c{};
        if (!hub::parse_mac(txt_value(r, "mac"), c.mac)) continue;
        if (std::memcmp(c.mac, g_mac, 6) == 0) continue;  // our own answer
        const char* pref = txt_value(r, "hub");
        c.preferred      = pref && strcmp(pref, "1") == 0;
        g_roster.saw(c, now);
    }
    mdns_query_results_free(res);
}

// Hold or drop the alias; follow an IP change while holding it.
void apply_alias(bool hold, uint32_t ip, uint32_t& held_ip) {
    const bool held = g_alias_held.load(std::memory_order_relaxed) != 0;
    if (hold && ip) {
        mdns_ip_addr_t a = self_addr(ip);
        if (!held) {
            if (mdns_delegate_hostname_add(hub::kAlias, &a) != ESP_OK) return;
            ESP_LOGI(TAG, "mDNS: %s.local now points here", hub::kAlias);
        } else if (ip != held_ip) {
            mdns_delegate_hostname_set_address(hub::kAlias, &a);
        }
        held_ip = ip;
        g_alias_held.store(1, std::memory_order_relaxed);
    } else if (held) {
        mdns_delegate_hostname_remove(hub::kAlias);
        g_alias_held.store(0, std::memory_order_relaxed);
        ESP_LOGI(TAG, "mDNS: %s.local released to a sibling", hub::kAlias);
    }
}

// Once a second: republish the TXT items the user can change; every
// kBrowseMs (and right after such a change) browse and run the election.
void web_hub_task(void*) {
    uint32_t held_ip = 0, last_browse = 0;
    bool first                                          = true;
    char node[sizeof(config::GlobalConfig::short_name)] = "";
    int pref                                            = -1;
    while (g_hub_run.load(std::memory_order_acquire)) {
        const auto& g = config::get_global();
        bool changed  = false;
        if (std::strncmp(node, g.short_name, sizeof(node)) != 0) {
            std::snprintf(node, sizeof(node), "%s", g.short_name);
            mdns_instance_name_set(node);
            mdns_service_txt_item_set("_http", "_tcp", "node", node);
            changed = true;
        }
        if (pref != (g.hub_preferred ? 1 : 0)) {
            pref = g.hub_preferred ? 1 : 0;
            mdns_service_txt_item_set("_http", "_tcp", "hub", pref ? "1" : "0");
            changed = true;
        }
        const uint32_t now = now_ms();
        if (first || changed || now - last_browse >= hub::kBrowseMs) {
            browse_siblings();
            last_browse = now;
            first       = false;
        }
        const uint32_t t = now_ms();
        g_siblings.store(static_cast<uint32_t>(g_roster.live(t)), std::memory_order_relaxed);
        apply_alias(g_roster.holds_alias(self_candidate(), t), ui::get_ip(), held_ip);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    g_hub_alive.store(0, std::memory_order_release);
    vTaskDelete(nullptr);
}

void start_mdns() {
    if (g_mdns_up) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    esp_read_mac(g_mac, ESP_MAC_ETH);
    hub::hostname_for(g_mac, g_hostname, sizeof(g_hostname));
    mdns_hostname_set(g_hostname);
    const auto& g = config::get_global();
    mdns_instance_name_set(g.short_name);
    char mac[13];
    hub::format_mac(g_mac, mac);
    // TXT record lets the aggregated UI tell pixfrogs apart from other
    // _http._tcp hosts on the LAN, and carries what the election needs.
    mdns_txt_item_t txt[5] = {
        { "product", "pixfrog" },
        { "node", g.short_name },
        { "fw", esp_app_get_description()->version },
        { "mac", mac },
        { "hub", g.hub_preferred ? "1" : "0" },
    };
    mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 5);
    g_mdns_up = true;
    g_roster.clear();
    g_alias_held.store(0, std::memory_order_relaxed);
    g_hub_run.store(1, std::memory_order_release);
    g_hub_alive.store(1, std::memory_order_release);
    if (xTaskCreate(web_hub_task, "web_hub", 4096, nullptr, 2, nullptr) != pdPASS) {
        g_hub_run.store(0, std::memory_order_relaxed);
        g_hub_alive.store(0, std::memory_order_relaxed);
    }
    ESP_LOGI(TAG, "mDNS: %s.local", g_hostname);
}

const char* mdns_host() {
    return g_mdns_up ? g_hostname : "";
}
bool alias_held() {
    return g_alias_held.load(std::memory_order_relaxed) != 0;
}
uint32_t sibling_count() {
    return g_siblings.load(std::memory_order_relaxed);
}

void stop_mdns() {
    if (!g_mdns_up) return;
    g_hub_run.store(0, std::memory_order_release);
    // The task may be inside a 750 ms browse: let it leave before mdns_free.
    for (int i = 0; i < 60 && g_hub_alive.load(std::memory_order_acquire); ++i)
        vTaskDelay(pdMS_TO_TICKS(50));
    mdns_free();
    g_alias_held.store(0, std::memory_order_relaxed);
    g_mdns_up = false;
}

// ── GET /api/peers ──────────────────────────────────────────────────────────
// Browse the LAN for other pixfrogs (mDNS PTR on _http._tcp, filtered by the
// product=pixfrog TXT record) so the SPA can build the multi-node UI. Self is
// always listed first with self:true. The mDNS browse blocks, so the JSON is
// cached briefly to keep the SPA's periodic poll from stalling the server.

static void build_peers_json(char* out, size_t cap) {
    char self_ip[16]   = "";
    const uint32_t cur = ui::get_ip();
    if (cur) fmt_ip(self_ip, sizeof(self_ip), cur);
    const auto& g = config::get_global();

    char seen[16][24];  // "ip:port" already listed
    size_t nseen = 0;
    snprintf(seen[nseen++], sizeof(seen[0]), "%s:80", self_ip);

    cJSON* arr = cJSON_CreateArray();
    cJSON* me  = cJSON_CreateObject();
    cJSON_AddStringToObject(me, "name", g.short_name);
    cJSON_AddStringToObject(me, "ip", self_ip);
    cJSON_AddNumberToObject(me, "port", 80);
    cJSON_AddStringToObject(me, "fw", esp_app_get_description()->version);
    cJSON_AddBoolToObject(me, "self", true);
    char mac_hex[13];
    hub::format_mac(g_mac, mac_hex);
    cJSON_AddStringToObject(me, "host", mdns_host());
    cJSON_AddStringToObject(me, "mac", mac_hex);
    cJSON_AddBoolToObject(me, "hub", g.hub_preferred != 0);
    cJSON_AddItemToArray(arr, me);
    // Which listed box holds pixfrog.local (same rule as the election).
    hub::Candidate best = self_candidate();
    cJSON* best_item    = me;

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
            char key[24];
            snprintf(key, sizeof(key), "%s:%u", ip, r->port ? r->port : 80);
            bool dup = false;
            for (size_t i = 0; i < nseen; ++i)
                if (strcmp(seen[i], key) == 0) {
                    dup = true;
                    break;
                }
            if (dup || nseen >= 16) continue;
            snprintf(seen[nseen++], sizeof(seen[0]), "%s", key);

            const char* node = txt_value(r, "node");
            const char* fw   = txt_value(r, "fw");
            cJSON* o         = cJSON_CreateObject();
            cJSON_AddStringToObject(
                o, "name", node ? node : (r->instance_name ? r->instance_name : "pixfrog"));
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddNumberToObject(o, "port", r->port ? r->port : 80);
            cJSON_AddStringToObject(o, "fw", fw ? fw : "");
            cJSON_AddBoolToObject(o, "self", false);
            hub::Candidate c{};
            if (hub::parse_mac(txt_value(r, "mac"), c.mac)) {
                const char* pref = txt_value(r, "hub");
                c.preferred      = pref && strcmp(pref, "1") == 0;
                char host[hub::kHostnameMax];
                hub::hostname_for(c.mac, host, sizeof(host));
                cJSON_AddStringToObject(o, "host", host);
                cJSON_AddStringToObject(o, "mac", txt_value(r, "mac"));
                cJSON_AddBoolToObject(o, "hub", c.preferred);
                if (hub::outranks(c, best)) {
                    best      = c;
                    best_item = o;
                }
            }
            cJSON_AddItemToArray(arr, o);
        }
        mdns_query_results_free(res);
    }
    cJSON_AddBoolToObject(best_item, "alias", true);

    char* str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    snprintf(out, cap, "%s", str ? str : "[]");
    if (str) cJSON_free(str);
}

esp_err_t handle_get_peers(httpd_req_t* req) {
    static char cache[4096];  // 16 boxes × ~200 B
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

}  // namespace pixfrog::web::impl
