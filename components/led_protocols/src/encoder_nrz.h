// Internal: NRZ (1-wire) encoder + shared per-pixel transform helpers.

#pragma once

#include <array>

#include "led_protocols.h"

namespace pixfrog::led::detail {

// Reorder a raw RGB(W) triple/quad into the strip's wire order.
// `in` always carries R,G,B[,W] in source order; the strip protocol may want
// GRB or BGR. We resolve order at encode time so the upstream pipeline stays
// uniform (artnet → dmx_manager → encoder).
struct Reorder {
    uint8_t r, g, b, w;
};

// Per order, the source channel (0 R, 1 G, 2 B, 3 W) of each wire position,
// read off its name at compile time.
struct OrderSlots {
    uint8_t s[4];
};
constexpr OrderSlots order_slots_of(const char* name) {
    OrderSlots o{ { 0, 1, 2, 3 } };
    for (int k = 0; k < 4 && name[k]; ++k)
        o.s[k] = name[k] == 'R' ? 0 : name[k] == 'G' ? 1 : name[k] == 'B' ? 2 : 3;
    return o;
}
constexpr std::array<OrderSlots, static_cast<size_t>(ColorOrder::COUNT)> make_order_slots() {
    std::array<OrderSlots, static_cast<size_t>(ColorOrder::COUNT)> a{};
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = order_slots_of(kColorOrderNames[i]);
    return a;
}
inline constexpr auto kOrderSlots = make_order_slots();
static_assert(kOrderSlots[static_cast<size_t>(ColorOrder::GRB)].s[0] == 1 &&
                  kOrderSlots[static_cast<size_t>(ColorOrder::WRGB)].s[0] == 3,
              "slots follow the names");

// The four bytes in wire order (a 3-byte protocol sends the first three).
inline Reorder apply_order(ColorOrder order, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    const uint8_t src[4] = { r, g, b, w };
    const auto i         = static_cast<size_t>(order);
    if (i >= kOrderSlots.size()) return { r, g, b, w };
    const uint8_t* s = kOrderSlots[i].s;
    return { src[s[0]], src[s[1]], src[s[2]], src[s[3]] };
}

inline uint8_t apply_brightness(uint8_t component, uint8_t brightness) {
    // Cheap 8-bit gain ≈ component * brightness / 255. (x * (b+1)) >> 8 keeps
    // brightness 255 an exact identity (the previous (x*b+128)>>8 capped full
    // white at 0xfe — measured on the wire) and stays within 1 LSB of exact.
    return static_cast<uint8_t>((static_cast<uint16_t>(component) * (brightness + 1)) >> 8);
}

// Source byte pointer for output pixel `out_pi`, honouring direction inversion
// and grouping. `bytes_per_px` is the SOURCE stride (3 for RGB, 4 for RGBW).
// Returns null for a dead physical position (see ChannelDesc::gaps): invert and
// grouping act on the live pixels only, gaps stay where they are wired.
inline const uint8_t* source_pixel(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                                   size_t bytes_per_px) {
    uint32_t live_pi = out_pi;
    uint32_t live_n  = desc.pixel_count;
    if (desc.gap_count) {
        const int32_t li = live_index(out_pi, desc.gaps, desc.gap_count);
        if (li < 0) return nullptr;
        live_pi = static_cast<uint32_t>(li);
        live_n  = live_within(desc.pixel_count, desc.gaps, desc.gap_count);
    }
    const uint32_t src_pi = desc.invert_direction ? live_n - 1 - live_pi : live_pi;
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
    if (!p) {
        out3[0] = out3[1] = out3[2] = 0;
        return;
    }
    out3[0] = corrected(desc, desc.lut ? desc.lut->r : nullptr, p[0]);
    out3[1] = corrected(desc, desc.lut ? desc.lut->g : nullptr, p[1]);
    out3[2] = corrected(desc, desc.lut ? desc.lut->b : nullptr, p[2]);
}

// Resolve output pixel `out_pi` to its wire-order, brightness-scaled bytes,
// honouring grouping and direction inversion. Shared by the per-channel NRZ
// encoder and the single-pass frame encoder so both emit identical streams.
inline void transformed_pixel_bytes(const ChannelDesc& desc, const uint8_t* pixels, uint16_t out_pi,
                                    uint8_t out_bytes[4]) {
    const size_t bytes_per_px = bytes_per_pixel(desc.protocol);
    const uint8_t* p          = source_pixel(desc, pixels, out_pi, bytes_per_px);
    if (!p) {
        out_bytes[0] = out_bytes[1] = out_bytes[2] = out_bytes[3] = 0;
        return;
    }

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
