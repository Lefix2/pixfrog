// Internal: DMX512 output encoder (one universe, async serial @ 250 kbps).
//
// Unlike the LED encoders, DMX512 is not a pixel protocol: the source bytes
// are raw DMX slot values that are framed as standard 8N2 UART characters and
// preceded by a BREAK + Mark-After-Break + null start code. The waveform is
// emitted on the channel's DATA bit; the CLOCK bit stays low (DMX is 1-wire).

#pragma once

#include "led_protocols.h"

namespace pixfrog::led::detail {

// DMX512-A waveform constants at f_PCLK = 16 MHz (see docs/PROTOCOLS.md §7).
constexpr uint16_t kDmxSamplesPerBit = kPclkHz / 250'000u;  // 64 samples = 4 µs/bit
constexpr uint16_t kDmxBreakSamples  = 1536;                // 96 µs (spec ≥ 88 µs)
constexpr uint16_t kDmxMabSamples    = 192;                 // 12 µs (spec ≥ 8 µs)
constexpr uint16_t kDmxBitsPerSlot   = 11;                  // 1 start + 8 data + 2 stop

// Worst-case sample count for `slot_count` data slots (+ the null start code).
size_t dmx_encoded_size_samples(uint16_t slot_count);

size_t encode_dmx(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity);

}  // namespace pixfrog::led::detail
