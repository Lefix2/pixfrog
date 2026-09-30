#pragma once
#include <cstddef>
#include <cstdint>
void esp_fill_random(void* buf, size_t len);
uint32_t esp_random();
