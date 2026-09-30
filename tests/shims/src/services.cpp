// OTA slots, core-dump partition, mDNS browse results, base64, heap figures.
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
    std::string instance, product, node, fw;
    uint32_t ip;
};
std::vector<Peer> g_peers;
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
                   const char* fw) {
    g_peers.push_back({ instance, product ? product : "", node ? node : "", fw ? fw : "", ip });
}
void mdns_reset() {
    g_peers.clear();
}
}  // namespace shim

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    return &kOta1;
}
esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t* out) {
    g_ota.clear();
    g_ota_open      = true;
    g_boot_switched = false;
    *out            = 1;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void* data, size_t size) {
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
    return ESP_OK;
}
void mdns_free() {}
esp_err_t mdns_hostname_set(const char*) {
    return ESP_OK;
}
esp_err_t mdns_instance_name_set(const char*) {
    return ESP_OK;
}
esp_err_t mdns_service_add(const char*, const char*, const char*, uint16_t, mdns_txt_item_t*,
                           size_t) {
    return ESP_OK;
}
esp_err_t mdns_query_ptr(const char*, const char*, uint32_t, size_t, mdns_result_t** results) {
    mdns_result_t* head = nullptr;
    for (auto it = g_peers.rbegin(); it != g_peers.rend(); ++it) {
        auto* r            = new mdns_result_t{};
        r->instance_name   = it->instance.c_str();
        r->port            = 80;
        r->txt             = new mdns_txt_item_t[3]{ { "product", it->product.c_str() },
                                                     { "node", it->node.c_str() },
                                                     { "fw", it->fw.c_str() } };
        r->txt_count       = 3;
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
