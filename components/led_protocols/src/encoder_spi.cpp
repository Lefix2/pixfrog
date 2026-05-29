#include "encoder_spi.h"

namespace pixfrog::led::detail {

namespace {

uint8_t apply_brightness_spi(uint8_t component, uint8_t brightness) {
    return static_cast<uint8_t>((static_cast<uint16_t>(component) * brightness + 128) >> 8);
}

// Emit one bit (`bit_value`) cadenced on CLOCK. Returns number of samples written.
// Layout per CLOCK cycle (samples_per_clock = spc, must be even):
//   first  spc/2 samples: CLOCK = 0, DATA = bit_value
//   second spc/2 samples: CLOCK = 1, DATA = bit_value
size_t emit_clocked_bit(uint16_t* out, uint16_t data_mask, uint16_t clk_mask,
                        bool bit_value, uint16_t spc) {
    const uint16_t half = spc / 2;
    const uint16_t data_set = bit_value ? data_mask : 0;
    for (uint16_t k = 0; k < half; ++k) {
        out[k] |= data_set;                 // CLOCK low half
    }
    for (uint16_t k = half; k < spc; ++k) {
        out[k] |= data_set | clk_mask;      // CLOCK high half
    }
    return spc;
}

size_t emit_clocked_byte(uint16_t* out, uint16_t data_mask, uint16_t clk_mask,
                         uint8_t byte, uint16_t spc) {
    size_t s = 0;
    for (int bit = 7; bit >= 0; --bit) {
        s += emit_clocked_bit(out + s, data_mask, clk_mask, (byte >> bit) & 1, spc);
    }
    return s;
}

}  // namespace

size_t encode_spi(const ChannelDesc& desc,
                  const uint8_t*     pixels,
                  uint16_t*          out_samples,
                  size_t             out_samples_capacity) {
    const Timing t = timing_for(desc.protocol, desc.clock_hz);
    const uint16_t data_mask = static_cast<uint16_t>(1u << desc.bus_bit_data);
    const uint16_t clk_mask  = static_cast<uint16_t>(1u << desc.bus_bit_clock);
    const uint16_t spc       = t.samples_per_clock;
    const size_t needed = encoded_size_samples(desc);
    if (out_samples_capacity < needed) return 0;

    size_t s = 0;

    // APA102 / SK9822 start frame: 4 × 0x00.
    if (desc.protocol == Protocol::APA102 || desc.protocol == Protocol::SK9822) {
        for (int i = 0; i < 4; ++i) s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0x00, spc);
    }

    const uint16_t px_max = desc.pixel_count;
    for (uint16_t pi = 0; pi < px_max; ++pi) {
        const uint16_t src_pi   = desc.invert_direction ? (px_max - 1 - pi) : pi;
        const uint16_t group    = desc.grouping ? desc.grouping : 1;
        const uint16_t group_src = src_pi / group;
        const uint8_t* p = pixels + group_src * 3;

        const uint8_t r = apply_brightness_spi(p[0], desc.brightness);
        const uint8_t g = apply_brightness_spi(p[1], desc.brightness);
        const uint8_t b = apply_brightness_spi(p[2], desc.brightness);

        switch (desc.protocol) {
            case Protocol::APA102:
            case Protocol::SK9822:
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0xE0 | 0x1F, spc); // brightness = max
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, b, spc);
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, g, spc);
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, r, spc);
                break;
            case Protocol::LPD8806:
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0x80 | (g >> 1), spc);
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0x80 | (r >> 1), spc);
                s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0x80 | (b >> 1), spc);
                break;
            default:
                break;
        }
    }

    // End frame: enough CLOCK pulses to propagate the last pixel through the chain.
    if (desc.protocol == Protocol::APA102 || desc.protocol == Protocol::SK9822) {
        const size_t end_bytes = (px_max + 15) / 16 + 1;
        for (size_t i = 0; i < end_bytes; ++i) {
            s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0xFF, spc);
        }
    } else if (desc.protocol == Protocol::LPD8806) {
        const size_t latch_bytes = (px_max + 31) / 32;
        for (size_t i = 0; i < latch_bytes; ++i) {
            s += emit_clocked_byte(out_samples + s, data_mask, clk_mask, 0x00, spc);
        }
    }

    return s;
}

}  // namespace pixfrog::led::detail
