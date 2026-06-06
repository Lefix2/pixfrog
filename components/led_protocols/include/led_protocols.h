// led_protocols — per-protocol pixel-to-DMA-samples encoders.
//
// This component is the pure compute core: given a pixel buffer, a protocol
// descriptor, and a target sample buffer, it emits the parallel-bus 16-bit
// samples that drive the LCD_CAM peripheral.
//
// It has no IDF or hardware dependency and is unit-testable on a host.
//
// See docs/PROTOCOLS.md for timing tables and PCLK derivation.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixfrog::led {

// ────────────────────────────────────────────────────────────────────────────
// Protocol enumeration
// ────────────────────────────────────────────────────────────────────────────

enum class Protocol : uint8_t {
    WS2815  = 0,
    WS2812B = 1,
    WS2811  = 2,
    SK6812  = 3,
    WS2814  = 4,
    APA102  = 5,
    SK9822  = 6,
    LPD8806 = 7,
    DMX512  = 8,
    COUNT,
};

constexpr bool is_clocked(Protocol p) {
    return p == Protocol::APA102 || p == Protocol::SK9822 || p == Protocol::LPD8806;
}

constexpr bool is_rgbw(Protocol p) {
    return p == Protocol::SK6812 || p == Protocol::WS2814;
}

// DMX512 raw output: the channel emits one DMX universe (8N2 @ 250 kbps) on its
// DATA bit rather than encoding RGB(W) pixels. Source bytes are passed through.
constexpr bool is_dmx(Protocol p) {
    return p == Protocol::DMX512;
}

// Source bytes per logical "pixel". For DMX512 one "pixel" is a single DMX slot.
constexpr size_t bytes_per_pixel(Protocol p) {
    if (is_dmx(p)) return 1;
    return is_rgbw(p) ? 4 : 3;
}

// ────────────────────────────────────────────────────────────────────────────
// Color order
// ────────────────────────────────────────────────────────────────────────────

enum class ColorOrder : uint8_t {
    RGB,
    RBG,
    GRB,
    GBR,
    BRG,
    BGR,
    RGBW,
    GRBW,
    RGBWW,
    COUNT,
};

// ────────────────────────────────────────────────────────────────────────────
// Timing — derived from f_PCLK (cf. docs/PROTOCOLS.md §3)
// ────────────────────────────────────────────────────────────────────────────

// PCLK frequency the LCD_CAM bus is configured at, system-wide.
// Changing this requires recomputing every Timing::samples_* constant.
constexpr uint32_t kPclkHz = 16'000'000;

// Worst-case sample count for one frame, used to size the PSRAM frame buffer.
// 1024 px × 32 bits (RGBW) × 40 samples (WS2811-slow) + 4480 (TRESET).
// At 2 bytes per sample, this gives ~2.6 MB per FB → ~5.2 MB total for 2 FBs.
constexpr uint32_t kMaxPixelsPerChannel = 1024;
constexpr uint32_t kMaxBitsPerPixel     = 32;
constexpr uint32_t kMaxSamplesPerBit    = 40;
constexpr uint32_t kMaxResetSamples     = 4480;
constexpr uint32_t kMaxSamplesPerFrame  = kMaxPixelsPerChannel * kMaxBitsPerPixel *
                                             kMaxSamplesPerBit +
                                         kMaxResetSamples;

// Samples per logical "thing" — varies by protocol family.
struct Timing {
    // For 1-wire NRZ: samples high to encode '0' and '1', total samples per bit.
    uint16_t samples_t0h;
    uint16_t samples_t1h;
    uint16_t samples_bit;  // = T0H + T0L = T1H + T1L (within ±1 sample)

    // For clocked SPI-like: samples per full CLOCK cycle (must be even, ≥ 2).
    uint16_t samples_per_clock;

    // Frame-reset samples to keep DATA low after the last bit (latches the strip).
    uint32_t samples_reset;
};

// Returns a baked-in timing for the given protocol at compile time.
// `requested_clock_hz` is used only for clocked protocols and clamped to PCLK/2.
Timing timing_for(Protocol p, uint32_t requested_clock_hz = 4'000'000);

// ────────────────────────────────────────────────────────────────────────────
// Channel descriptor for the encoder
// ────────────────────────────────────────────────────────────────────────────

struct ChannelDesc {
    Protocol protocol;
    ColorOrder color_order;
    uint16_t pixel_count;  // logical pixels in the source buffer
    uint8_t brightness;    // 0..255, applied at encode time
    uint8_t grouping;      // 1..8; N source pixels share one output pixel
    bool invert_direction;
    uint8_t bus_bit_data;   // which bit of the 16-bit bus carries DATA   (0..15)
    uint8_t bus_bit_clock;  // which bit carries CLOCK (clocked protocols only)
    uint32_t clock_hz;      // requested CLOCK rate (clocked only)
};

// ────────────────────────────────────────────────────────────────────────────
// Encoder entry point
// ────────────────────────────────────────────────────────────────────────────

// Encode `pixels` (length = pixel_count × bytes_per_pixel) into `out_samples`
// using the channel's protocol. `out_samples` must be at least
// `encoded_size_samples(desc)` words wide. Existing bits in `out_samples`
// outside this channel's bus_bit_data/bus_bit_clock are preserved (OR-ed),
// allowing 8 channels to share the same DMA buffer.
//
// Returns the number of samples actually written.
size_t encode_channel(const ChannelDesc& desc, const uint8_t* pixels, uint16_t* out_samples,
                      size_t out_samples_capacity);

// Worst-case sample count for `desc`, used for buffer sizing.
size_t encoded_size_samples(const ChannelDesc& desc);

}  // namespace pixfrog::led
