#include "encoder_nrz.h"

namespace pixfrog::led::detail {

size_t encode_nrz(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity) {
    const Timing t            = timing_for(desc.protocol, desc.clock_hz);
    const uint16_t bit_mask   = static_cast<uint16_t>(1u << desc.bus_bit_data);
    const size_t bytes_per_px = bytes_per_pixel(desc.protocol);
    const size_t needed       = encoded_size_samples(desc);
    if (out_samples_capacity < needed) return 0;

    size_t s              = 0;
    const uint16_t px_max = desc.pixel_count;

    for (uint16_t pi = 0; pi < px_max; ++pi) {
        uint8_t bytes[4];
        transformed_pixel_bytes(desc, pixels, pi, bytes);

        for (size_t bi = 0; bi < bytes_per_px; ++bi) {
            const uint8_t byte = bytes[bi];
            for (int bit = 7; bit >= 0; --bit) {
                const bool one              = (byte >> bit) & 1u;
                const uint16_t high_samples = one ? t.samples_t1h : t.samples_t0h;
                const uint16_t total        = t.samples_bit;
                for (uint16_t k = 0; k < high_samples; ++k) {
                    out_samples[s++] |= bit_mask;
                }
                // remaining samples already 0-init'd in DMA buffer (caller zeroes)
                s += (total - high_samples);
            }
        }
    }

    // Reset / latch: keep DATA at 0 for samples_reset samples.
    s += t.samples_reset;

    return s;
}

}  // namespace pixfrog::led::detail
