// Internal: NRZ (1-wire) encoder + shared per-pixel transform helpers.

#pragma once

#include "led_protocols.h"

namespace pixfrog::led::detail {

// Reorder a raw RGB(W) triple/quad into the strip's wire order.
// `in` always carries R,G,B[,W] in source order; the strip protocol may want
// GRB or BGR. We resolve order at encode time so the upstream pipeline stays
// uniform (artnet → dmx_manager → encoder).
struct Reorder {
    uint8_t r, g, b, w;
};

inline Reorder apply_order(ColorOrder order, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    Reorder out{};
    switch (order) {
    case ColorOrder::RGB: out = { r, g, b, w }; break;
    case ColorOrder::RBG: out = { r, b, g, w }; break;
    case ColorOrder::GRB: out = { g, r, b, w }; break;
    case ColorOrder::GBR: out = { g, b, r, w }; break;
    case ColorOrder::BRG: out = { b, r, g, w }; break;
    case ColorOrder::BGR: out = { b, g, r, w }; break;
    case ColorOrder::RGBW: out = { r, g, b, w }; break;
    case ColorOrder::GRBW: out = { g, r, b, w }; break;
    default: out = { r, g, b, w }; break;
    }
    return out;
}

inline uint8_t apply_brightness(uint8_t component, uint8_t brightness) {
    // Cheap 8-bit gain ≈ component * brightness / 255. (x * (b+1)) >> 8 keeps
    // brightness 255 an exact identity (the previous (x*b+128)>>8 capped full
    // white at 0xfe — measured on the wire) and stays within 1 LSB of exact.
    return static_cast<uint8_t>((static_cast<uint16_t>(component) * (brightness + 1)) >> 8);
}

// Resolve output pixel `out_pi` to its wire-order, brightness-scaled bytes,
// honouring grouping and direction inversion. Shared by the per-channel NRZ
// encoder and the single-pass frame encoder so both emit identical streams.
inline void transformed_pixel_bytes(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                                    uint8_t out_bytes[4]) {
    const size_t bytes_per_px = bytes_per_pixel(desc.protocol);
    const uint16_t src_pi     = desc.invert_direction
                                  ? static_cast<uint16_t>(desc.pixel_count - 1 - out_pi)
                                  : out_pi;
    const uint16_t group      = desc.grouping ? desc.grouping : 1;
    const uint8_t* p          = pixels + (src_pi / group) * bytes_per_px;

    const uint8_t r = p[0];
    const uint8_t g = p[1];
    const uint8_t b = p[2];
    const uint8_t w = (bytes_per_px == 4) ? p[3] : 0;

    const Reorder o = apply_order(desc.color_order, r, g, b, w);
    out_bytes[0]    = apply_brightness(o.r, desc.brightness);
    out_bytes[1]    = apply_brightness(o.g, desc.brightness);
    out_bytes[2]    = apply_brightness(o.b, desc.brightness);
    out_bytes[3]    = apply_brightness(o.w, desc.brightness);
}

size_t encode_nrz(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity);

}  // namespace pixfrog::led::detail
