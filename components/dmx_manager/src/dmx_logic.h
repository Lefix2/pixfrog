// dmx_logic — pure (header-only) helpers shared between dmx_manager.cpp
// and its host-side unit tests.
//
// Everything here is constexpr/inline-eligible and depends only on
// led_protocols and config_store struct definitions. No IDF symbols.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "config_store.h"
#include "led_protocols.h"

namespace pixfrog::dmx::logic {

constexpr size_t kUniverseSize = 512;

// ── Sizing helpers ──────────────────────────────────────────────────────────

inline size_t channel_total_bytes(const config::ChannelConfig& cc) {
    return static_cast<size_t>(cc.pixel_count) * led::bytes_per_pixel(cc.protocol);
}

inline size_t channel_universes_used(const config::ChannelConfig& cc) {
    const size_t total = channel_total_bytes(cc);
    return (total + kUniverseSize - 1) / kUniverseSize;
}

// ── Capacity check ──────────────────────────────────────────────────────────

inline uint64_t channel_t_dma_us(const config::ChannelConfig& cc, uint32_t pclk_hz) {
    led::ChannelDesc d{};
    d.protocol    = cc.protocol;
    d.pixel_count = cc.pixel_count;
    d.clock_hz    = cc.clock_hz;
    if (pclk_hz == 0) return 0;
    return static_cast<uint64_t>(led::encoded_size_samples(d)) * 1'000'000ULL / pclk_hz;
}

inline uint64_t emission_budget_us(uint8_t refresh_rate_hz, uint64_t reserve_us = 1000) {
    if (refresh_rate_hz == 0) return 0;
    const uint64_t period = 1'000'000ULL / refresh_rate_hz;
    return (period > reserve_us) ? (period - reserve_us) : period;
}

inline bool channel_fits_budget(const config::ChannelConfig& cc, uint32_t pclk_hz,
                                uint64_t budget_us) {
    return channel_t_dma_us(cc, pclk_hz) <= budget_us;
}

// ── Pixel-count preview pattern ─────────────────────────────────────────────
//
// Strip visualization while the user edits a channel's pixel count:
//   LED i in [1..N):  white 10%   (orientation / extent)
//   LED i % 10 == 0:  white 50%   (decade ruler marks)
//   LED N:            white 100%  (the count being edited)
// Levels are baked into the buffer; the caller bypasses per-channel
// brightness so the pattern reads the same on every strip.

constexpr uint8_t kPreviewLevelLow  = 26;   // 10 %
constexpr uint8_t kPreviewLevelMid  = 128;  // 50 %
constexpr uint8_t kPreviewLevelFull = 255;  // 100 %

inline void fill_preview_pattern(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                                 uint8_t bytes_per_pixel) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0) return;
    for (uint16_t i = 1; i <= pixel_count; ++i) {
        uint8_t level = kPreviewLevelLow;
        if (i % 10 == 0) level = kPreviewLevelMid;
        if (i == pixel_count) level = kPreviewLevelFull;
        std::memset(dst + static_cast<size_t>(i - 1) * bytes_per_pixel, level, bytes_per_pixel);
    }
}

// ── Signal-loss failsafe ────────────────────────────────────────────────────
//
// A channel is "due" for failsafe when it has been active at least once
// (last_activity_us != 0 — a never-driven channel is already black and must
// not light a fallback scene) and its last packet is older than the timeout.
// timeout_s == 0 disables the feature entirely.

inline bool failsafe_due(int64_t last_activity_us, int64_t now_us, uint16_t timeout_s) {
    if (timeout_s == 0 || last_activity_us == 0) return false;
    return (now_us - last_activity_us) > static_cast<int64_t>(timeout_s) * 1'000'000;
}

// Fill the pixel buffer with the failsafe pattern. Blackout zeroes the
// channel; solid colour writes r,g,b per pixel (any extra bytes — W on RGBW
// strips — stay 0 so the white die is not driven blind).
inline void fill_failsafe_pattern(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                                  uint8_t bytes_per_pixel, uint8_t mode, uint8_t r, uint8_t g,
                                  uint8_t b) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0) return;
    if (mode != 2 /* colour */) {
        std::memset(dst, 0, total);
        return;
    }
    for (uint16_t i = 0; i < pixel_count; ++i) {
        uint8_t* p = dst + static_cast<size_t>(i) * bytes_per_pixel;
        p[0]       = r;
        if (bytes_per_pixel > 1) p[1] = g;
        if (bytes_per_pixel > 2) p[2] = b;
        for (uint8_t k = 3; k < bytes_per_pixel; ++k)
            p[k] = 0;
    }
}

// ── Pixel decoder ───────────────────────────────────────────────────────────
//
// Copies bytes from one or more universes into the destination buffer,
// applying `cc.dmx_start` as the 1-based offset into the first universe.
// `get_universe(universe_number)` must return a 512-byte buffer or nullptr.
// On any missing universe, the remainder of `dst` is zero-filled and the
// function returns false (the strip stays in a defined state).

template <typename GetUniverseFn>
inline bool decode_pixels(uint8_t* dst, size_t dst_capacity, const config::ChannelConfig& cc,
                          GetUniverseFn get_universe) {
    const size_t total = channel_total_bytes(cc);
    if (total > dst_capacity) return false;
    if (total == 0) return true;

    const uint16_t start_dmx = cc.dmx_start > 0 ? cc.dmx_start : 1;
    size_t offset_in_uni     = start_dmx - 1;
    uint16_t universe        = cc.universe_start;
    size_t bytes_written     = 0;

    while (bytes_written < total) {
        const uint8_t* src = get_universe(universe);
        if (!src) {
            std::memset(dst + bytes_written, 0, total - bytes_written);
            return false;
        }
        const size_t available = (kUniverseSize > offset_in_uni) ? (kUniverseSize - offset_in_uni)
                                                                 : 0;
        const size_t need      = total - bytes_written;
        const size_t copy      = need < available ? need : available;
        if (copy == 0) {
            universe++;
            offset_in_uni = 0;
            continue;
        }
        std::memcpy(dst + bytes_written, src + offset_in_uni, copy);
        bytes_written += copy;
        universe++;
        offset_in_uni = 0;
    }
    return true;
}

}  // namespace pixfrog::dmx::logic
