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
    const uint16_t data_mask = static_cast<uint16_t>(1u << desc.bus_bit_data);
    // The channel's CLOCK bit is unused by DMX, so we drive it as the logical
    // complement of DATA. With the board's two lines per channel this yields a
    // complementary pair (DATA+/DATA−): a differential DMX receiver then sees a
    // genuine ±Vcc swing — polarity-correct BREAK and bits — instead of the
    // single-ended signal a lone DATA line gives. It is NOT a true EIA-485
    // driver (no termination drive, common-mode range or fault protection); a
    // real RS-485 transceiver is still preferable for long/terminated runs.
    // See docs/PROTOCOLS.md §7.4.
    const uint16_t comp_mask = static_cast<uint16_t>(1u << desc.bus_bit_clock);
    const size_t needed      = dmx_encoded_size_samples(desc.pixel_count);
    if (out_samples_capacity < needed) return 0;

    size_t s = 0;

    // Emit `n` samples of the differential pair at a given logic level. The
    // caller pre-zeroes the buffer, so only the asserted line is written.
    auto emit = [&](uint16_t n, bool high) {
        const uint16_t m = high ? data_mask : comp_mask;
        for (uint16_t k = 0; k < n; ++k)
            out_samples[s++] |= m;
    };

    // BREAK: line LOW for kDmxBreakSamples.
    emit(kDmxBreakSamples, false);
    // Mark-After-Break: line HIGH.
    emit(kDmxMabSamples, true);

    // One 8N2 character: start bit LOW, 8 data bits LSB-first, 2 stop bits HIGH.
    auto emit_char = [&](uint8_t value) {
        emit(kDmxSamplesPerBit, false);  // start bit (LOW)
        for (int bit = 0; bit < 8; ++bit)
            emit(kDmxSamplesPerBit, (value >> bit) & 1u);
        emit(static_cast<uint16_t>(2 * kDmxSamplesPerBit), true);  // 2 stop bits (HIGH)
    };

    emit_char(0x00);  // null start code (standard DMX dimmer data)
    for (uint16_t i = 0; i < desc.pixel_count; ++i)
        emit_char(pixels[i]);

    return s;
}

}  // namespace pixfrog::led::detail
