// Host shim: cache maintenance is a no-op on the host (can be made to fail).
#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#define ESP_CACHE_MSYNC_FLAG_INVALIDATE (1 << 0)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED (1 << 1)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 2)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C (1 << 3)

esp_err_t esp_cache_msync(void* addr, size_t size, int flags);
