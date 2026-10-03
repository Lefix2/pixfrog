// Host shim: the ESP32 EMAC + IP101 PHY driver. Construction can be made to
// fail step by step; link events are posted by the test (shim::event_post).
#pragma once
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

typedef struct esp_eth_driver_t* esp_eth_handle_t;
typedef struct esp_eth_mac_s {
    int unused;
} esp_eth_mac_t;
typedef struct esp_eth_phy_s {
    int unused;
} esp_eth_phy_t;

typedef struct {
    uint32_t sw_reset_timeout_ms;
    uint32_t rx_task_stack_size;
    uint32_t rx_task_prio;
    uint32_t flags;
} eth_mac_config_t;
typedef struct {
    struct {
        int mdc_num;
        int mdio_num;
    } smi_gpio;
    int interface;
} eth_esp32_emac_config_t;
typedef struct {
    int32_t phy_addr;
    uint32_t reset_timeout_ms;
    uint32_t autonego_timeout_ms;
    int reset_gpio_num;
} eth_phy_config_t;
typedef struct {
    esp_eth_mac_t* mac;
    esp_eth_phy_t* phy;
    uint32_t check_link_period_ms;
} esp_eth_config_t;

#define ETH_MAC_DEFAULT_CONFIG()                                                                   \
    eth_mac_config_t {                                                                             \
        100, 4096, 15, 0                                                                           \
    }
#define ETH_ESP32_EMAC_DEFAULT_CONFIG()                                                            \
    eth_esp32_emac_config_t {                                                                      \
        { 31, 52 }, 0                                                                              \
    }
#define ETH_PHY_DEFAULT_CONFIG()                                                                   \
    eth_phy_config_t {                                                                             \
        -1, 100, 4000, 5                                                                           \
    }
#define ETH_DEFAULT_CONFIG(emac, ephy)                                                             \
    esp_eth_config_t {                                                                             \
        emac, ephy, 2000                                                                           \
    }

extern const esp_event_base_t ETH_EVENT;
typedef enum {
    ETHERNET_EVENT_START,
    ETHERNET_EVENT_STOP,
    ETHERNET_EVENT_CONNECTED,
    ETHERNET_EVENT_DISCONNECTED,
} eth_event_t;

esp_eth_mac_t* esp_eth_mac_new_esp32(const eth_esp32_emac_config_t* esp32_cfg,
                                     const eth_mac_config_t* cfg);
esp_eth_phy_t* esp_eth_phy_new_ip101(const eth_phy_config_t* cfg);
esp_err_t esp_eth_driver_install(const esp_eth_config_t* cfg, esp_eth_handle_t* out);
esp_netif_iodriver_handle esp_eth_new_netif_glue(esp_eth_handle_t eth);
esp_err_t esp_eth_start(esp_eth_handle_t eth);
typedef enum { ETH_CMD_S_ALL_MULTICAST = 18 } esp_eth_io_cmd_t;
esp_err_t esp_eth_ioctl(esp_eth_handle_t eth, esp_eth_io_cmd_t cmd, void* data);
