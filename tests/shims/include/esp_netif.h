// Host shim: one Ethernet netif; what the firmware configures is recorded
// (shim::netif_log).
#pragma once
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"

typedef struct esp_netif_obj esp_netif_t;
typedef struct {
    int unused;
} esp_netif_config_t;
#define ESP_NETIF_DEFAULT_ETH()                                                                    \
    esp_netif_config_t {                                                                           \
        0                                                                                          \
    }

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

extern const esp_event_base_t IP_EVENT;
enum { IP_EVENT_ETH_GOT_IP = 5, IP_EVENT_ETH_LOST_IP = 6 };
typedef struct {
    esp_netif_t* esp_netif;
    esp_netif_ip_info_t ip_info;
    bool ip_changed;
} ip_event_got_ip_t;

typedef void* esp_netif_iodriver_handle;

esp_err_t esp_netif_init();
esp_netif_t* esp_netif_new(const esp_netif_config_t* cfg);
esp_err_t esp_netif_attach(esp_netif_t* netif, esp_netif_iodriver_handle glue);
esp_err_t esp_netif_dhcpc_stop(esp_netif_t* netif);
esp_err_t esp_netif_dhcpc_start(esp_netif_t* netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t* netif, const esp_netif_ip_info_t* info);
