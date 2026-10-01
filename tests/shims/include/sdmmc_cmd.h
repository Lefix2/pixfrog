// Host shim: an SD card is present when the test inserted one
// (shim::sd_insert); its status follows.
#pragma once
#include "esp_err.h"

typedef struct sdmmc_card_t {
    int unused;
} sdmmc_card_t;

esp_err_t sdmmc_get_status(sdmmc_card_t* card);
