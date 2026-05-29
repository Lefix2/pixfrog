// Internal: clocked SPI-like encoder (APA102 / SK9822 / LPD8806).

#pragma once

#include "led_protocols.h"

namespace pixfrog::led::detail {

size_t encode_spi(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity);

}  // namespace pixfrog::led::detail
