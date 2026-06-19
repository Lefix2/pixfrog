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

// Source byte pointer for output pixel `out_pi`, honouring direction inversion
// and grouping. `bytes_per_px` is the SOURCE stride (3 for RGB, 4 for RGBW).
inline const uint8_t* source_pixel(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                                   size_t bytes_per_px) {
    const uint16_t src_pi = desc.invert_direction
                              ? static_cast<uint16_t>(desc.pixel_count - 1 - out_pi)
                              : out_pi;
    const uint16_t group  = desc.grouping ? desc.grouping : 1;
    return pixels + static_cast<size_t>(src_pi / group) * bytes_per_px;
}

// One source component through its gamma/white-balance LUT (when present) then
// the brightness gain. `lut_table` is the per-component table or null.
inline uint8_t corrected(const ChannelDesc& desc, const uint8_t* lut_table, uint8_t v) {
    if (lut_table) v = lut_table[v];
    return apply_brightness(v, desc.brightness);
}

// RGB source bytes for a clocked-protocol pixel (grouping/invert + LUT +
// brightness), left in source order — clocked encoders assemble their own
// native wire order (APA102 BGR, LPD8806 GRB), so no colour-order remap here.
// Shared by encode_spi and the single-pass clocked sweep.
inline void transformed_rgb(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                            uint8_t out3[3]) {
    const uint8_t* p = source_pixel(desc, pixels, out_pi, 3);
    out3[0]          = corrected(desc, desc.lut ? desc.lut->r : nullptr, p[0]);
    out3[1]          = corrected(desc, desc.lut ? desc.lut->g : nullptr, p[1]);
    out3[2]          = corrected(desc, desc.lut ? desc.lut->b : nullptr, p[2]);
}

// Resolve output pixel `out_pi` to its wire-order, brightness-scaled bytes,
// honouring grouping and direction inversion. Shared by the per-channel NRZ
// encoder and the single-pass frame encoder so both emit identical streams.
inline void transformed_pixel_bytes(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                                    uint8_t out_bytes[4]) {
    const size_t bytes_per_px = bytes_per_pixel(desc.protocol);
    const uint8_t* p          = source_pixel(desc, pixels, out_pi, bytes_per_px);

    // LUT + brightness up front, then the colour-order permutation. Brightness
    // is a per-component scale, so applying it before the permutation is
    // identical to after — and lets the LUT+brightness step be the one shared
    // with the clocked encoders (see transformed_rgb / corrected).
    const uint8_t r = corrected(desc, desc.lut ? desc.lut->r : nullptr, p[0]);
    const uint8_t g = corrected(desc, desc.lut ? desc.lut->g : nullptr, p[1]);
    const uint8_t b = corrected(desc, desc.lut ? desc.lut->b : nullptr, p[2]);
    const uint8_t w = corrected(desc, desc.lut ? desc.lut->w : nullptr,
                                bytes_per_px == 4 ? p[3] : 0);

    const Reorder o = apply_order(desc.color_order, r, g, b, w);
    out_bytes[0]    = o.r;
    out_bytes[1]    = o.g;
    out_bytes[2]    = o.b;
    out_bytes[3]    = o.w;
}

size_t encode_nrz(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                  size_t out_samples_capacity);

}  // namespace pixfrog::led::detail
