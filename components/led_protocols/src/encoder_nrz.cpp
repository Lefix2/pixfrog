#include "encoder_nrz.h"

namespace pixfrog::led::detail {

namespace {

// Reorder a raw RGB(W) triple/quad into the strip's wire order.
// `in` always carries R,G,B[,W] in source order; the strip protocol may want
// GRB or BGR. We resolve order at encode time so the upstream pipeline stays
// uniform (artnet → dmx_manager → encoder).
struct Reorder {
    uint8_t r, g, b, w;
};

Reorder apply_order(ColorOrder order, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    Reorder out{};
    switch (order) {
        case ColorOrder::RGB:  out = {r, g, b, w}; break;
        case ColorOrder::RBG:  out = {r, b, g, w}; break;
        case ColorOrder::GRB:  out = {g, r, b, w}; break;
        case ColorOrder::GBR:  out = {g, b, r, w}; break;
        case ColorOrder::BRG:  out = {b, r, g, w}; break;
        case ColorOrder::BGR:  out = {b, g, r, w}; break;
        case ColorOrder::RGBW: out = {r, g, b, w}; break;
        case ColorOrder::GRBW: out = {g, r, b, w}; break;
        default:               out = {r, g, b, w}; break;
    }
    return out;
}

uint8_t apply_brightness(uint8_t component, uint8_t brightness) {
    // Cheap 8-bit gain: result = component * brightness / 255.
    // Using (x * b + 128) / 256 ≈ x * b / 255 with single-rounding bias.
    return static_cast<uint8_t>((static_cast<uint16_t>(component) * brightness + 128) >> 8);
}

}  // namespace

size_t encode_nrz(const ChannelDesc& desc,
                  const uint8_t*     pixels,
                  uint16_t*          out_samples,
                  size_t             out_samples_capacity) {
    const Timing t = timing_for(desc.protocol, desc.clock_hz);
    const uint16_t bit_mask = static_cast<uint16_t>(1u << desc.bus_bit_data);
    const size_t bytes_per_px = bytes_per_pixel(desc.protocol);
    const size_t needed = encoded_size_samples(desc);
    if (out_samples_capacity < needed) return 0;

    size_t s = 0;
    const uint16_t px_max = desc.pixel_count;

    for (uint16_t pi = 0; pi < px_max; ++pi) {
        const uint16_t src_pi = desc.invert_direction ? (px_max - 1 - pi) : pi;
        // Apply grouping: each output pixel reads from src_pi / grouping.
        const uint16_t group = desc.grouping ? desc.grouping : 1;
        const uint16_t group_src = src_pi / group;
        const uint8_t* p = pixels + group_src * bytes_per_px;

        uint8_t r = p[0];
        uint8_t g = p[1];
        uint8_t b = p[2];
        uint8_t w = (bytes_per_px == 4) ? p[3] : 0;

        const Reorder o = apply_order(desc.color_order, r, g, b, w);

        uint8_t bytes[4];
        bytes[0] = apply_brightness(o.r, desc.brightness);
        bytes[1] = apply_brightness(o.g, desc.brightness);
        bytes[2] = apply_brightness(o.b, desc.brightness);
        bytes[3] = apply_brightness(o.w, desc.brightness);

        for (size_t bi = 0; bi < bytes_per_px; ++bi) {
            const uint8_t byte = bytes[bi];
            for (int bit = 7; bit >= 0; --bit) {
                const bool one = (byte >> bit) & 1u;
                const uint16_t high_samples = one ? t.samples_t1h : t.samples_t0h;
                const uint16_t total = t.samples_bit;
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
