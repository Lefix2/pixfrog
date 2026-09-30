#pragma once
#include "esp_err.h"
#include <cstdint>
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
