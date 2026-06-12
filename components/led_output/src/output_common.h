// Helpers shared by the two LED bus output backends (led_output.cpp and
// parlio_output.cpp): config→descriptor mapping, frame sizing, calibration
// pattern fills, and the GPIO bit-bang bring-up probe. Everything here is
// backend-agnostic and side-effect free except the GPIO probe.

#pragma once

#include <cstring>

#include "driver/gpio.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"

namespace pixfrog::output::common {

// Per-channel gamma/white-balance LUTs, rebuilt lazily when the source
// values change (4-byte compare per frame; powf only on a config commit).
// C++17 inline variables: one instance shared by both backends.
struct LutCacheEntry {
    led::PixelLut lut;
    uint32_t key = 0;  // gamma_x10 | wb_r<<8 | wb_g<<16 | wb_b<<24; 0 = never built
};
inline LutCacheEntry g_lut_cache[config::kNumChannels];

inline const led::PixelLut* channel_lut(size_t ch, const config::ChannelConfig& cc) {
    const bool identity = (cc.gamma_x10 <= 10) && cc.wb_r == 255 && cc.wb_g == 255 &&
                          cc.wb_b == 255;
    if (identity) return nullptr;  // skip the per-byte lookups entirely
    const uint32_t key = static_cast<uint32_t>(cc.gamma_x10) |
                         (static_cast<uint32_t>(cc.wb_r) << 8) |
                         (static_cast<uint32_t>(cc.wb_g) << 16) |
                         (static_cast<uint32_t>(cc.wb_b) << 24);
    auto& e = g_lut_cache[ch];
    if (e.key != key) {
        led::build_pixel_lut(e.lut, cc.gamma_x10, cc.wb_r, cc.wb_g, cc.wb_b);
        e.key = key;
    }
    return &e.lut;
}

// Build a led::ChannelDesc from the current ChannelConfig + bus bit assignment.
inline led::ChannelDesc desc_for_channel(size_t ch) {
    const auto& cc = config::get_channel(ch);
    led::ChannelDesc d{};
    d.protocol         = cc.protocol;
    d.color_order      = cc.color_order;
    d.pixel_count      = cc.pixel_count;
    d.brightness       = cc.brightness;
    d.grouping         = cc.grouping;
    d.invert_direction = cc.invert_direction;
    d.bus_bit_data     = static_cast<uint8_t>(ch * 2);
    d.bus_bit_clock    = static_cast<uint8_t>(ch * 2 + 1);
    d.clock_hz         = cc.clock_hz;
    d.lut              = channel_lut(ch, cc);

    // Pixel-count preview: emit exactly the previewed number of physical
    // LEDs with the pattern's own levels — neutralize per-channel transforms
    // so LED N on the wire is LED N on the screen.
    if (dmx::pixel_preview_channel() == static_cast<int>(ch)) {
        d.pixel_count      = dmx::pixel_preview_count();
        d.brightness       = 255;
        d.grouping         = 1;
        d.invert_direction = false;
    }
    return d;
}

// Frame length the current config needs: the longest channel (incl. its
// reset tail). Channels share the bus in parallel, so this is a max, not a
// sum. Pure arithmetic — cheap enough to evaluate every frame.
inline size_t required_frame_samples() {
    size_t mx = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const led::ChannelDesc d = desc_for_channel(ch);
        if (led::is_off(d.protocol)) continue;
        const size_t len = led::encoded_size_samples(d);
        if (len > mx) mx = len;
    }
    return mx;
}

inline size_t round_up(size_t v, size_t q) {
    return (v + q - 1) / q * q;
}

inline size_t gcd(size_t a, size_t b) {
    while (b != 0) {
        const size_t t = a % b;
        a              = b;
        b              = t;
    }
    return a;
}

inline size_t lcm(size_t a, size_t b) {
    return a / gcd(a, b) * b;
}

// Scope-validation patterns (cal 0..2) — fill `count` bus samples.
inline void fill_calibration_pattern(uint16_t* samples, size_t count, uint8_t pattern_id,
                                     uint32_t pclk_hz) {
    switch (pattern_id) {
    case 0: {
        // 1 kHz square wave on all 16 bits: half-period = pclk/1000/2 samples.
        const size_t half = pclk_hz / 2000;
        for (size_t i = 0; i < count; ++i) {
            const bool high = ((i / half) & 1) == 0;
            samples[i]      = high ? 0xFFFFu : 0x0000u;
        }
        break;
    }
    case 1: {
        // Walking-1 across the 16 bits, holding each high for 256 samples
        // (= 16 µs at PCLK=16 MHz — comfortably scope-able).
        constexpr size_t per_bit = 256;
        for (size_t i = 0; i < count; ++i) {
            samples[i] = static_cast<uint16_t>(1u << ((i / per_bit) % 16));
        }
        break;
    }
    case 2: {
        // 0xAAAA on even samples, 0x5555 on odd samples — every sample
        // flips every bit, useful to see if PCLK is correct.
        for (size_t i = 0; i < count; ++i) {
            samples[i] = (i & 1) ? 0x5555u : 0xAAAAu;
        }
        break;
    }
    default: std::memset(samples, 0, count * sizeof(uint16_t)); break;
    }
}

// Hardware bring-up probe (cal 3): drive the 16 bus pins through the plain
// GPIO driver, bypassing the output peripheral entirely. Separates
// "peripheral emits nothing" from "pin/probe wiring dead". Steals the pins
// from the peripheral via the GPIO matrix — reboot to restore normal output.
// Call once per render tick; toggles all pins each call.
inline void gpio_bitbang_probe_tick(const int* bus_gpio_16) {
    static bool s_gpio_owned = false;
    if (!s_gpio_owned) {
        for (int i = 0; i < 16; ++i) {
            const gpio_num_t g = static_cast<gpio_num_t>(bus_gpio_16[i]);
            gpio_reset_pin(g);
            gpio_set_direction(g, GPIO_MODE_OUTPUT);
        }
        s_gpio_owned = true;
    }
    static bool s_level = false;
    s_level             = !s_level;
    for (int i = 0; i < 16; ++i) {
        gpio_set_level(static_cast<gpio_num_t>(bus_gpio_16[i]), s_level ? 1 : 0);
    }
}

}  // namespace pixfrog::output::common
