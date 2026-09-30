#pragma once
#include "esp_err.h"
#include <cstddef>
esp_err_t esp_core_dump_image_get(size_t* out_addr, size_t* out_size);
esp_err_t esp_core_dump_image_erase();
