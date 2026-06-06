#include "encoder_dmx.h"

namespace pixfrog::led::detail {

size_t dmx_encoded_size_samples(uint16_t slot_count) {
    // BREAK + MAB, then the null start code plus every data slot as an 8N2
    // character (11 bit-times each). The line idles HIGH (mark) between slots,
    // which the stop bits already provide.
    const size_t chars = static_cast<size_t>(slot_count) + 1;  // +1 = start code
    return static_cast<size_t>(kDmxBreakSamples) + kDmxMabSamples +
           chars * kDmxBitsPerSlot * kDmxSamplesPerBit;
}

size_t encode_dmx(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity) {
    const uint16_t bit_mask = static_cast<uint16_t>(1u << desc.bus_bit_data);
    const size_t needed     = dmx_encoded_size_samples(desc.pixel_count);
    if (out_samples_capacity < needed) return 0;

    size_t s = 0;

    // Drive the DATA bit HIGH for `n` samples; the caller pre-zeroes the buffer
    // so LOW segments are produced simply by advancing `s`.
    auto mark = [&](uint16_t n) {
        for (uint16_t k = 0; k < n; ++k) out_samples[s++] |= bit_mask;
    };

    // BREAK: line LOW (already zeroed) for kDmxBreakSamples.
    s += kDmxBreakSamples;
    // Mark-After-Break: line HIGH.
    mark(kDmxMabSamples);

    // One 8N2 character: start bit LOW, 8 data bits LSB-first, 2 stop bits HIGH.
    auto emit_char = [&](uint8_t value) {
        s += kDmxSamplesPerBit;  // start bit (LOW)
        for (int bit = 0; bit < 8; ++bit) {
            if ((value >> bit) & 1u)
                mark(kDmxSamplesPerBit);
            else
                s += kDmxSamplesPerBit;
        }
        mark(static_cast<uint16_t>(2 * kDmxSamplesPerBit));  // 2 stop bits (HIGH)
    };

    emit_char(0x00);  // null start code (standard DMX dimmer data)
    for (uint16_t i = 0; i < desc.pixel_count; ++i)
        emit_char(pixels[i]);

    return s;
}

}  // namespace pixfrog::led::detail
