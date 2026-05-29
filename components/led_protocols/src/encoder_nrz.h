// Internal: NRZ (1-wire) encoder.

#pragma once

#include "led_protocols.h"

namespace pixfrog::led::detail {

size_t encode_nrz(const ChannelDesc& desc,
                  const uint8_t*     pixels,
                  uint16_t*          out_samples,
                  size_t             out_samples_capacity);

}  // namespace pixfrog::led::detail
