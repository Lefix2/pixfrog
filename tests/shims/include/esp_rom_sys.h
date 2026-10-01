// Host shim: ROM busy-wait advances the fake clock.
#pragma once
#include <cstdint>

void esp_rom_delay_us(uint32_t us);
