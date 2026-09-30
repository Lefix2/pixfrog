#pragma once
#include "esp_app_desc.h"
#include "esp_err.h"
#include <cstdint>
typedef struct {
    char label[17];
    uint32_t address;
    uint32_t size;
} esp_partition_t;
const esp_partition_t* esp_ota_get_running_partition();
