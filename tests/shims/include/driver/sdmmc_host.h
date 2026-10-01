// Host shim: the SDMMC host/slot configuration types fseq_player fills in.
#pragma once
#include <cstdint>

#include "esp_err.h"

typedef int gpio_num_t;

typedef struct {
    int max_freq_khz;
    int slot;
} sdmmc_host_t;

typedef struct {
    gpio_num_t clk, cmd, d0, d1, d2, d3;
    int width;
    uint32_t flags;
} sdmmc_slot_config_t;

#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_HOST_DEFAULT()                                                                       \
    sdmmc_host_t {                                                                                 \
        20000, 1                                                                                   \
    }
#define SDMMC_SLOT_CONFIG_DEFAULT()                                                                \
    sdmmc_slot_config_t {                                                                          \
        0, 0, 0, 0, 0, 0, 4, 0                                                                     \
    }
