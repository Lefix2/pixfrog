// mDNS subset: announcements are recorded, browse results come from peers the
// test registers (shim::mdns_add_peer).
#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#define ESP_IPADDR_TYPE_V4 0
#define ESP_IPADDR_TYPE_V6 6
typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
    union {
        esp_ip4_addr_t ip4;
    } u_addr;
    uint8_t type;
} esp_ip_addr_t;
typedef struct mdns_ip_addr_s {
    esp_ip_addr_t addr;
    struct mdns_ip_addr_s* next;
} mdns_ip_addr_t;
typedef struct {
    const char* key;
    const char* value;
} mdns_txt_item_t;
typedef struct mdns_result_s {
    struct mdns_result_s* next;
    const char* instance_name;
    const char* hostname;
    uint16_t port;
    mdns_txt_item_t* txt;
    size_t txt_count;
    mdns_ip_addr_t* addr;
} mdns_result_t;
esp_err_t mdns_init();
void mdns_free();
esp_err_t mdns_hostname_set(const char* name);
esp_err_t mdns_instance_name_set(const char* name);
esp_err_t mdns_service_add(const char* instance, const char* service, const char* proto,
                           uint16_t port, mdns_txt_item_t* txt, size_t n);
esp_err_t mdns_query_ptr(const char* service, const char* proto, uint32_t timeout,
                         size_t max_results, mdns_result_t** results);
void mdns_query_results_free(mdns_result_t* results);
