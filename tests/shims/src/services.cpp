// OTA slots, core-dump partition, mDNS browse results, base64, heap figures.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "shim_control.h"

namespace {
std::vector<uint8_t> g_ota;
bool g_ota_open = false, g_boot_switched = false, g_ota_fail = false;
std::vector<uint8_t> g_coredump;
const esp_partition_t kOta1 = { "ota_1", 0x720000, 0x700000 };
const esp_partition_t kCore = { "coredump", 0xE20000, 0x10000 };

struct Peer {
    std::string instance, product, node, fw, mac, hub;
    uint32_t ip;
    uint16_t port;
};
std::vector<Peer> g_peers;
std::string g_mdns_host;
std::map<std::string, uint32_t> g_delegates;  // host-order IPv4
std::map<std::string, std::string> g_txt;

uint32_t host_order(const mdns_ip_addr_t* a) {
    const uint32_t n = a ? a->addr.u_addr.ip4.addr : 0;
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) | ((n >> 8) & 0xFF00) | (n >> 24);
}
}  // namespace

namespace shim {
std::vector<uint8_t>& ota_image() {
    return g_ota;
}
bool ota_boot_switched() {
    return g_boot_switched;
}
void ota_fail_validation(bool fail) {
    g_ota_fail = fail;
}
void coredump_set(const std::vector<uint8_t>& image) {
    g_coredump = image;
}
void mdns_add_peer(const char* instance, uint32_t ip, const char* product, const char* node,
                   const char* fw, const char* mac, const char* hub, uint16_t port) {
    g_peers.push_back({ instance, product ? product : "", node ? node : "", fw ? fw : "",
                        mac ? mac : "", hub ? hub : "", ip, port });
}
std::string mdns_hostname() {
    return g_mdns_host;
}
uint32_t mdns_delegate_ip(const char* hostname) {
    auto it = g_delegates.find(hostname);
    return it == g_delegates.end() ? 0 : it->second;
}
std::string mdns_txt(const char* key) {
    auto it = g_txt.find(key);
    return it == g_txt.end() ? std::string() : it->second;
}
void mdns_reset() {
    g_peers.clear();
}
}  // namespace shim

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    if (shim::should_fail(shim::Fault::OtaNoTarget)) return nullptr;
    return &kOta1;
}
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t* out) {
    if (shim::should_fail(shim::Fault::OtaBegin)) return ESP_FAIL;
    g_ota.clear();
    g_ota_open      = true;
    g_boot_switched = false;
    *out            = 1;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void* data, size_t size) {
    if (shim::should_fail(shim::Fault::OtaWrite)) return ESP_FAIL;
    if (!g_ota_open) return ESP_ERR_INVALID_ARG;
    const auto* p = static_cast<const uint8_t*>(data);
    g_ota.insert(g_ota.end(), p, p + size);
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t) {
    g_ota_open = false;
    // A real image starts with the 0xE9 magic byte.
    if (g_ota_fail || g_ota.empty() || g_ota[0] != 0xE9) return ESP_ERR_OTA_VALIDATE_FAILED;
    return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t) {
    g_ota_open = false;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) {
    if (shim::should_fail(shim::Fault::OtaSetBoot)) return ESP_FAIL;
    g_boot_switched = true;
    return ESP_OK;
}
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t,
                                                const char*) {
    return &kCore;
}
esp_err_t esp_partition_read(const esp_partition_t*, size_t off, void* dst, size_t size) {
    if (off + size > g_coredump.size()) return ESP_ERR_INVALID_SIZE;
    std::memcpy(dst, g_coredump.data() + off, size);
    return ESP_OK;
}
esp_err_t esp_core_dump_image_get(size_t* out_addr, size_t* out_size) {
    if (g_coredump.empty()) return ESP_ERR_NOT_FOUND;
    *out_addr = kCore.address;
    *out_size = g_coredump.size();
    return ESP_OK;
}
esp_err_t esp_core_dump_image_erase() {
    g_coredump.clear();
    return ESP_OK;
}

uint32_t esp_get_free_heap_size() {
    return 24u << 20;
}
uint32_t esp_get_minimum_free_heap_size() {
    return 23u << 20;
}

// ── mDNS ─────────────────────────────────────────────────────────────────────

esp_err_t mdns_init() {
    if (shim::should_fail(shim::Fault::MdnsInit)) return ESP_FAIL;
    return ESP_OK;
}
void mdns_free() {
    g_delegates.clear();
    g_txt.clear();
}
esp_err_t mdns_hostname_set(const char* name) {
    g_mdns_host = name;
    return ESP_OK;
}
esp_err_t mdns_delegate_hostname_add(const char* hostname, const mdns_ip_addr_t* a) {
    if (g_delegates.count(hostname)) return ESP_ERR_INVALID_ARG;
    g_delegates[hostname] = host_order(a);
    return ESP_OK;
}
esp_err_t mdns_delegate_hostname_set_address(const char* hostname, const mdns_ip_addr_t* a) {
    if (!g_delegates.count(hostname)) return ESP_ERR_NOT_FOUND;
    g_delegates[hostname] = host_order(a);
    return ESP_OK;
}
esp_err_t mdns_delegate_hostname_remove(const char* hostname) {
    g_delegates.erase(hostname);
    return ESP_OK;
}
esp_err_t mdns_service_txt_item_set(const char*, const char*, const char* key, const char* value) {
    g_txt[key] = value;
    return ESP_OK;
}
esp_err_t mdns_instance_name_set(const char*) {
    return ESP_OK;
}
esp_err_t mdns_service_add(const char*, const char*, const char*, uint16_t, mdns_txt_item_t* txt,
                           size_t n) {
    for (size_t i = 0; i < n; ++i)
        g_txt[txt[i].key] = txt[i].value;
    return ESP_OK;
}
esp_err_t mdns_query_ptr(const char*, const char*, uint32_t, size_t, mdns_result_t** results) {
    mdns_result_t* head = nullptr;
    for (auto it = g_peers.rbegin(); it != g_peers.rend(); ++it) {
        auto* r = new mdns_result_t{};
        r->port = it->port;
        // An empty node/fw is left out of the TXT record, as a peer without them would.
        r->txt                 = new mdns_txt_item_t[5]{};
        r->txt_count           = 0;
        r->txt[r->txt_count++] = { "product", it->product.c_str() };
        if (!it->node.empty()) r->txt[r->txt_count++] = { "node", it->node.c_str() };
        if (!it->fw.empty()) r->txt[r->txt_count++] = { "fw", it->fw.c_str() };
        if (!it->mac.empty()) r->txt[r->txt_count++] = { "mac", it->mac.c_str() };
        if (!it->hub.empty()) r->txt[r->txt_count++] = { "hub", it->hub.c_str() };
        r->instance_name   = it->instance.empty() ? nullptr : it->instance.c_str();
        r->addr            = new mdns_ip_addr_t{};
        r->addr->addr.type = ESP_IPADDR_TYPE_V4;
        // lwIP keeps IPv4 in network order: first octet in the low byte.
        r->addr->addr.u_addr.ip4.addr = ((it->ip >> 24) & 0xFF) | ((it->ip >> 8) & 0xFF00) |
                                        ((it->ip << 8) & 0xFF0000) | ((it->ip << 24) & 0xFF000000);
        r->next = head;
        head    = r;
    }
    *results = head;
    return ESP_OK;
}
void mdns_query_results_free(mdns_result_t* r) {
    while (r) {
        mdns_result_t* n = r->next;
        delete[] r->txt;
        delete r->addr;
        delete r;
        r = n;
    }
}

// ── base64 (RFC 4648) ────────────────────────────────────────────────────────

int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src,
                          size_t slen) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    uint32_t acc = 0;
    int bits     = 0;
    for (size_t i = 0; i < slen; ++i) {
        if (src[i] == '=') break;
        const int v = val(src[i]);
        if (v < 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        acc   = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>(acc >> bits));
        }
    }
    *olen = out.size();
    if (!dst || dlen < out.size()) return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    std::memcpy(dst, out.data(), out.size());
    return 0;
}
