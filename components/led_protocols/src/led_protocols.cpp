#include "led_protocols.h"

#include "encoder_nrz.h"
#include "encoder_spi.h"

namespace pixfrog::led {

Timing timing_for(Protocol p, uint32_t requested_clock_hz) {
    // Values derived in docs/PROTOCOLS.md §3.3 for f_PCLK = 16 MHz.
    // T_PCLK = 62.5 ns.
    Timing t{};
    switch (p) {
    case Protocol::WS2815:
    case Protocol::WS2814:
        t.samples_t0h   = 5;
        t.samples_t1h   = 15;
        t.samples_bit   = 20;
        t.samples_reset = 4480;  // 280 µs
        return t;
    case Protocol::WS2812B:
    case Protocol::WS2811:
        t.samples_t0h   = 6;
        t.samples_t1h   = 11;
        t.samples_bit   = 20;
        t.samples_reset = 800;  // 50 µs
        return t;
    case Protocol::SK6812:
        t.samples_t0h   = 5;
        t.samples_t1h   = 10;
        t.samples_bit   = 19;
        t.samples_reset = 1280;  // 80 µs
        return t;
    case Protocol::APA102:
    case Protocol::SK9822:
    case Protocol::LPD8806: {
        if (requested_clock_hz == 0) requested_clock_hz = 4'000'000;
        uint32_t spc = kPclkHz / requested_clock_hz;
        if (spc < 2) spc = 2;
        if (spc & 1) spc++;
        if (spc > 256) spc = 256;
        t.samples_per_clock = static_cast<uint16_t>(spc);
        return t;
    }
    default: return t;
    }
}

size_t encoded_size_samples(const ChannelDesc& desc) {
    const Timing t = timing_for(desc.protocol, desc.clock_hz);
    if (is_clocked(desc.protocol)) {
        // APA102/SK9822: 4-byte start + 4 bytes/pixel + ceil(pixels/2)/8 end bytes
        // LPD8806: 3 bytes/pixel + N/64 latch bytes
        // Use the larger of the two formulas for safety.
        const size_t bytes_per_px = (desc.protocol == Protocol::LPD8806) ? 3 : 4;
        const size_t start_bytes  = (desc.protocol == Protocol::LPD8806) ? 0 : 4;
        const size_t end_bytes    = (desc.pixel_count + 15) / 16 + 1;
        const size_t total_bytes  = start_bytes + bytes_per_px * desc.pixel_count + end_bytes;
        return total_bytes * 8 * t.samples_per_clock;
    } else {
        const size_t bits_per_px = bytes_per_pixel(desc.protocol) * 8;
        return static_cast<size_t>(desc.pixel_count) * bits_per_px * t.samples_bit +
               t.samples_reset;
    }
}

size_t encode_channel(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                      size_t out_samples_capacity) {
    if (is_clocked(desc.protocol)) {
        return detail::encode_spi(desc, pixels, out_samples, out_samples_capacity);
    }
    return detail::encode_nrz(desc, pixels, out_samples, out_samples_capacity);
}

}  // namespace pixfrog::led
