// Host shim: on-chip LDO channels (the VDD_IO_5 pad supply).
#pragma once
#include <cstdint>

#include "esp_err.h"

typedef struct ldo_unit_ctx_t* esp_ldo_channel_handle_t;
typedef struct {
    int chan_id;
    int voltage_mv;
    struct {
        uint32_t adjustable : 1;
        uint32_t owned_by_hw : 1;
        uint32_t bypass : 1;
    } flags;
} esp_ldo_channel_config_t;

esp_err_t esp_ldo_acquire_channel(const esp_ldo_channel_config_t* cfg,
                                  esp_ldo_channel_handle_t* out);
