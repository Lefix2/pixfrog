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

// Highest addressable universe number. Art-Net encodes the Port-Address on 15
// bits (Net<<8 | SubUni), so no universe above this can ever be routed — the
// universe→slot table is sized for exactly this range. sACN allows 1..63999 on
// the wire and FSEQ sparse ranges carry a 32-bit channel offset, so both must
// be filtered against this ceiling before they index anything.
constexpr uint16_t kMaxUniverseNumber = 0x7FFF;

inline bool universe_routable(uint32_t universe_number) {
    return universe_number <= kMaxUniverseNumber;
}

// Sentinel stored in the universe → slot table for "not mapped to any channel".
constexpr uint16_t kNoSlot = UINT16_MAX;

// ── Sizing helpers ──────────────────────────────────────────────────────────

inline size_t channel_total_bytes(const config::ChannelConfig& cc) {
    return static_cast<size_t>(cc.pixel_count) * led::bytes_per_pixel(cc.protocol);
}

inline size_t channel_universes_used(const config::ChannelConfig& cc) {
    const size_t total = channel_total_bytes(cc);
    return (total + kUniverseSize - 1) / kUniverseSize;
}

// ── Auto-patch (cascade universe assignment) ─────────────────────────────────
//
// Lay channels out contiguously from a flat 15-bit base universe: channel i is
// placed universe-aligned at the running cursor, which then advances by that
// channel's universe span (channel_universes_used). A channel spanning 0
// universes (disabled / 0 px) leaves the cursor where it is. The cursor is a
// flat 15-bit Port-Address, so it rolls through subnet/net boundaries naturally
// (universe 15 → 16 crosses into the next subnet). out[i] receives channel i's
// new universe_start; the caller resets dmx_start to 1 and persists. Returns
// the next free universe after the last channel (wrapped to 15 bits).
inline uint16_t compute_auto_patch(uint16_t base, const config::ChannelConfig* chans, size_t n,
                                   uint16_t* out) {
    uint32_t cursor = base;
    for (size_t i = 0; i < n; ++i) {
        out[i]  = static_cast<uint16_t>(cursor & 0x7FFF);
        cursor += channel_universes_used(chans[i]);
    }
    return static_cast<uint16_t>(cursor & 0x7FFF);
}

// ── Universe → slot map ─────────────────────────────────────────────────────
//
// Assign every channel's universe span a slot in the pool, and record the
// reverse slot → channel mapping. `uni_to_slot` must hold
// kMaxUniverseNumber + 1 entries and is filled with kNoSlot first;
// `slot_to_chan` must hold `num_slots` entries.
//
// Two ways a channel's span can fail to map, both reported through
// `out_unmapped` (number of universes that got no slot) rather than silently
// dropped: the pool runs out of slots, or the span would run past
// kMaxUniverseNumber (a channel patched near the top of the address space).
// Neither is allowed to write outside either array.
//
// Returns the number of slots used.
inline uint16_t build_universe_map(const config::ChannelConfig* chans, size_t n,
                                   uint16_t* uni_to_slot, uint8_t* slot_to_chan, size_t num_slots,
                                   size_t* out_unmapped) {
    for (size_t i = 0; i <= kMaxUniverseNumber; ++i)
        uni_to_slot[i] = kNoSlot;

    uint16_t slot   = 0;
    size_t unmapped = 0;
    for (size_t ch = 0; ch < n; ++ch) {
        const size_t universes_used = channel_universes_used(chans[ch]);
        for (size_t u = 0; u < universes_used; ++u) {
            const uint32_t uni = static_cast<uint32_t>(chans[ch].universe_start) + u;
            if (!universe_routable(uni) || slot >= num_slots) {
                unmapped++;
                continue;
            }
            uni_to_slot[uni]   = slot;
            slot_to_chan[slot] = static_cast<uint8_t>(ch);
            slot++;
        }
    }
    if (out_unmapped) *out_unmapped = unmapped;
    return slot;
}

// ── Capacity check ──────────────────────────────────────────────────────────

// Dead-pixel runs in use (none on an Off channel).
inline size_t channel_gap_count(const config::ChannelConfig& cc) {
    if (led::is_off(cc.protocol)) return 0;
    return led::gap_count(cc.gaps, led::kMaxPixelGaps);
}

// Pixels on the wire for the channel's live pixel_count: live + dead.
inline uint32_t channel_physical_pixels(const config::ChannelConfig& cc) {
    return led::physical_count(cc.pixel_count, cc.gaps, channel_gap_count(cc));
}

inline uint64_t channel_t_dma_us(const config::ChannelConfig& cc, uint32_t pclk_hz) {
    led::ChannelDesc d{};
    d.protocol    = cc.protocol;
    d.pixel_count = static_cast<uint16_t>(channel_physical_pixels(cc));
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

// The emission budget a channel is judged against at the configured refresh:
// the period minus the 1 ms encode-overlap reserve.
inline uint64_t channel_budget_us(const config::ChannelConfig&, uint8_t refresh_rate_hz) {
    return emission_budget_us(refresh_rate_hz);
}

// Capacity verdict for one channel at the configured refresh rate.
inline bool channel_fits_refresh(const config::ChannelConfig& cc, uint32_t pclk_hz,
                                 uint8_t refresh_rate_hz) {
    if (refresh_rate_hz == 0) return true;
    return channel_t_dma_us(cc, pclk_hz) <= channel_budget_us(cc, refresh_rate_hz);
}

// Inverse of channel_fits_refresh: the largest pixel_count whose encoded frame
// fits BOTH the DMA frame buffer (`buffer_samples`) and the per-channel
// emission budget at `refresh_rate_hz`. This is the cap the UI should enforce so
// a strip can never be configured longer than the bus can clock out in time —
// e.g. WS2815 tops out at 512 px @60 Hz (≈15.4 ms) but 1024 px @30 Hz fits.
//
// Bounded by led::kMaxPixelsPerChannel. Off channels have no constraint. Monotonic encoded size ⇒
// binary search.
inline uint16_t max_pixels_for(const config::ChannelConfig& cc, uint32_t pclk_hz,
                               uint8_t refresh_rate_hz, size_t buffer_samples) {
    if (led::is_off(cc.protocol) || pclk_hz == 0)
        return static_cast<uint16_t>(led::kMaxPixelsPerChannel);

    const uint16_t hard      = static_cast<uint16_t>(led::kMaxPixelsPerChannel);
    const uint64_t budget_us = channel_budget_us(cc, refresh_rate_hz);  // 0 ⇒ unbounded

    config::ChannelConfig probe = cc;
    uint16_t best = 1, lo = 1, hi = hard;
    while (lo <= hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        probe.pixel_count  = mid;
        led::ChannelDesc d{};
        d.protocol           = probe.protocol;
        d.pixel_count        = mid;
        d.clock_hz           = probe.clock_hz;
        const size_t samples = led::encoded_size_samples(d);
        const bool fits_buf  = samples <= buffer_samples;
        const bool fits_bud  = budget_us == 0 ||
                              static_cast<uint64_t>(samples) * 1'000'000ULL / pclk_hz <= budget_us;
        if (fits_buf && fits_bud) {
            best = mid;
            lo   = static_cast<uint16_t>(mid + 1);
        } else if (mid == 0) {
            break;
        } else {
            hi = static_cast<uint16_t>(mid - 1);
        }
    }
    return best;
}

// max_pixels_for counts physical pixels; this is the LIVE pixels that fit once
// the channel's dead pixels take their share of the line.
inline uint16_t max_live_pixels_for(const config::ChannelConfig& cc, uint32_t pclk_hz,
                                    uint8_t refresh_rate_hz, size_t buffer_samples) {
    const uint16_t phys = max_pixels_for(cc, pclk_hz, refresh_rate_hz, buffer_samples);
    return static_cast<uint16_t>(led::live_within(phys, cc.gaps, channel_gap_count(cc)));
}

// The live pixel count actually emitted: the stored count is what the user
// asked for and is never rewritten when the refresh rate (or the protocol /
// clock / gaps) shrinks the budget, so going back restores the full line.
inline uint16_t effective_pixel_count(const config::ChannelConfig& cc, uint32_t pclk_hz,
                                      uint8_t refresh_rate_hz, size_t buffer_samples) {
    if (led::is_off(cc.protocol)) return cc.pixel_count;
    const uint16_t mx = max_live_pixels_for(cc, pclk_hz, refresh_rate_hz, buffer_samples);
    return cc.pixel_count < mx ? cc.pixel_count : mx;
}

// ── Pixel-count preview pattern ─────────────────────────────────────────────
//
// Live "ruler" while the user edits a channel's pixel count. Colour encodes
// the scale so the strip is readable at a glance:
//   LED i in [1..N):   green  10%   (extent / orientation)
//   LED i % 10  == 0:  yellow 30%   (decade marks)
//   LED i % 100 == 0:  pink   30%   (centade marks, override decade)
//   LED N:             white 100%   (the count being edited)
// Colours are logical R,G,B (the encoder applies color_order); any W byte
// stays dark. The caller bypasses per-channel brightness so the pattern reads
// the same on every strip.
//
// `emit_count` (>= `lit_count`) is the number of physical LEDs to write: the
// pixels in (lit_count, emit_count] are blacked out. A shrinking count uses
// this to erase the pixels it just dropped — LEDs latch their last value, so
// one explicit black frame is what turns them off.

struct PreviewRGB {
    uint8_t r, g, b;
};
constexpr PreviewRGB kPreviewGreen{ 0, 26, 0 };       // 10 % green  — base / extent
constexpr PreviewRGB kPreviewYellow{ 77, 77, 0 };     // 30 % yellow — decade marks
constexpr PreviewRGB kPreviewPink{ 77, 0, 38 };       // 30 % pink   — centade marks
constexpr PreviewRGB kPreviewWhite{ 255, 255, 255 };  // 100 % white — the count

constexpr PreviewRGB kPreviewDead{ 90, 0, 0 };  // dim red — a dead pixel (gap)

// `lit_count` / `emit_count` count LIVE pixels; the buffer is written in
// physical order (the preview emits with no gap mapping) so each gap shows up
// on the strip as its own colour, exactly where it is wired.
inline void fill_preview_pattern(uint8_t* dst, size_t dst_capacity, uint16_t lit_count,
                                 uint16_t emit_count, uint8_t bytes_per_pixel,
                                 const led::PixelGap* gaps = nullptr, size_t gap_n = 0) {
    if (emit_count < lit_count) emit_count = lit_count;
    if (bytes_per_pixel == 0) return;
    uint32_t phys_emit      = led::physical_count(emit_count, gaps, gap_n);
    const uint32_t phys_lit = led::physical_count(lit_count, gaps, gap_n);
    // Without gaps an oversized request is a caller bug (write nothing, as
    // before); with gaps being edited it can outgrow the buffer — clip then.
    if (static_cast<size_t>(phys_emit) * bytes_per_pixel > dst_capacity) {
        if (gap_n == 0) return;
        phys_emit = static_cast<uint32_t>(dst_capacity / bytes_per_pixel);
    }
    for (uint32_t p = 0; p < phys_emit; ++p) {
        PreviewRGB c{ 0, 0, 0 };  // erase tail stays black
        const int32_t li = led::live_index(p, gaps, gap_n);
        if (li < 0) {
            if (p < phys_lit) c = kPreviewDead;
        } else {
            const uint32_t i = static_cast<uint32_t>(li) + 1;  // 1-based live LED
            if (i <= lit_count) {
                c = kPreviewGreen;
                if (i % 10 == 0) c = kPreviewYellow;
                if (i % 100 == 0) c = kPreviewPink;     // centade wins over decade
                if (i == lit_count) c = kPreviewWhite;  // the count wins over all
            }
        }
        uint8_t* px = dst + static_cast<size_t>(p) * bytes_per_pixel;
        px[0]       = c.r;
        if (bytes_per_pixel > 1) px[1] = c.g;
        if (bytes_per_pixel > 2) px[2] = c.b;
        for (uint8_t k = 3; k < bytes_per_pixel; ++k)
            px[k] = 0;  // W die stays dark
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

// ── Standalone scene generators ─────────────────────────────────────────────
//
// Parametric effects rendered straight into the channel's pixel back buffer
// (canonical RGB(W) order; colour order / brightness apply at encode time).
// `phase_ms` is wall-clock so animation speed is refresh-rate independent.

// Integer HSV→RGB, h in [0,360), s/v fixed at max — enough for a wheel.
inline void hue_to_rgb(uint32_t h360, uint8_t* r, uint8_t* g, uint8_t* b) {
    const uint32_t sector = (h360 % 360) / 60;
    const uint32_t f      = ((h360 % 360) % 60) * 255 / 60;  // 0..255 within sector
    const uint8_t q       = static_cast<uint8_t>(255 - f);
    const uint8_t t       = static_cast<uint8_t>(f);
    switch (sector) {
    case 0:
        *r = 255;
        *g = t;
        *b = 0;
        break;
    case 1:
        *r = q;
        *g = 255;
        *b = 0;
        break;
    case 2:
        *r = 0;
        *g = 255;
        *b = t;
        break;
    case 3:
        *r = 0;
        *g = q;
        *b = 255;
        break;
    case 4:
        *r = t;
        *g = 0;
        *b = 255;
        break;
    default:
        *r = 255;
        *g = 0;
        *b = q;
        break;
    }
}

inline void set_px(uint8_t* dst, uint8_t bpp, uint16_t i, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = dst + static_cast<size_t>(i) * bpp;
    p[0]       = r;
    if (bpp > 1) p[1] = g;
    if (bpp > 2) p[2] = b;
    for (uint8_t k = 3; k < bpp; ++k)
        p[k] = 0;  // W die off — scene colours are RGB-defined
}

struct Rgb {
    uint8_t r, g, b;
};

inline void set_px(uint8_t* dst, uint8_t bpp, uint16_t i, Rgb c) {
    set_px(dst, bpp, i, c.r, c.g, c.b);
}

// Saturating add, for effects whose shapes overlap (blobs).
inline void add_px(uint8_t* dst, uint8_t bpp, uint16_t i, Rgb c) {
    uint8_t* p         = dst + static_cast<size_t>(i) * bpp;
    const uint8_t v[3] = { c.r, c.g, c.b };
    for (uint8_t k = 0; k < 3 && k < bpp; ++k) {
        const unsigned s = p[k] + v[k];
        p[k]             = static_cast<uint8_t>(s > 255 ? 255 : s);
    }
}

inline Rgb scale_rgb(Rgb c, uint32_t v255) {
    return { static_cast<uint8_t>(c.r * v255 / 255), static_cast<uint8_t>(c.g * v255 / 255),
             static_cast<uint8_t>(c.b * v255 / 255) };
}

inline uint8_t lerp8(uint8_t a, uint8_t b, uint32_t t255) {
    return static_cast<uint8_t>(a +
                                (static_cast<int32_t>(b) - a) * static_cast<int32_t>(t255) / 255);
}

inline Rgb lerp_rgb(Rgb a, Rgb b, uint32_t t255) {
    return { lerp8(a.r, b.r, t255), lerp8(a.g, b.g, t255), lerp8(a.b, b.b, t255) };
}

// Stateless effects need per-pixel randomness that is stable across frames.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Smoothstep on 0..255: removes the corners of linear ramps.
inline uint8_t ease8(uint32_t t) {
    if (t > 255) t = 255;
    return static_cast<uint8_t>(t * t * (765 - 2 * t) / 65025);
}

// One cosine-like bump per period: 0 → 255 → 0 as phase runs 0..255.
inline uint8_t wave8(uint8_t phase) {
    const uint32_t t = phase < 128 ? phase * 2u : (255u - phase) * 2u;
    return ease8(t);
}

// 2-D value noise, coordinates in 1/256 cell units, result 0..255.
inline uint8_t noise8(uint32_t x, uint32_t y) {
    const uint32_t xi = x >> 8, yi = y >> 8;
    const int32_t fx = ease8(x & 255), fy = ease8(y & 255);
    auto corner = [](uint32_t cx, uint32_t cy) {
        return static_cast<int32_t>(hash32(cx * 73856093u ^ cy * 19349663u) & 255);
    };
    const int32_t c00 = corner(xi, yi), c10 = corner(xi + 1, yi);
    const int32_t c01 = corner(xi, yi + 1), c11 = corner(xi + 1, yi + 1);
    const int32_t top = c00 + (c10 - c00) * fx / 255;
    const int32_t bot = c01 + (c11 - c01) * fx / 255;
    return static_cast<uint8_t>(top + (bot - top) * fy / 255);
}

struct Palette {
    Rgb c[config::kSceneColorsMax];
    uint8_t n;
};

inline Palette scene_palette(const config::Scene& s) {
    Palette p{};
    p.n = config::scene_num_colors(s);
    for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
        uint8_t rgb[3];
        config::scene_color(s, k, rgb);
        p.c[k] = { rgb[0], rgb[1], rgb[2] };
    }
    return p;
}

// Cyclic gradient through the palette, one full cycle per 65536. A lone
// colour cycles against black so the gradient keeps some contrast.
inline Rgb palette_at(const Palette& p, uint32_t pos16) {
    const uint32_t n = p.n < 2 ? 2 : p.n;
    const uint32_t x = (pos16 & 0xFFFF) * n;
    const uint32_t k = x >> 16;
    const Rgb a      = p.n < 2 && k == 1 ? Rgb{ 0, 0, 0 } : p.c[k];
    const Rgb b      = p.n < 2 && k == 0 ? Rgb{ 0, 0, 0 } : p.c[(k + 1) % n];
    return lerp_rgb(a, b, (x & 0xFFFF) >> 8);
}

// Non-cyclic ramp black → colour 1 → … → colour n, for heat maps.
inline Rgb palette_ramp(const Palette& p, uint32_t v255) {
    const uint32_t x   = v255 * p.n;
    const uint32_t seg = x >> 8;
    const Rgb from     = seg == 0 ? Rgb{ 0, 0, 0 } : p.c[seg - 1];
    return lerp_rgb(from, p.c[seg < p.n ? seg : p.n - 1], x & 255);
}

// A bounce over [0, span] in 1/256 px: position at `travel256` along the path.
inline uint32_t bounce256(uint64_t travel256, uint32_t span) {
    const uint64_t leg = static_cast<uint64_t>(span) * 256;
    const uint64_t m   = travel256 % (2 * leg);
    return static_cast<uint32_t>(m < leg ? m : 2 * leg - m);
}

// Per-effect meaning of speed/param. param 0 always means the effect's default.
//   solid    — colour 1; speed = strobe 0..60 Hz: 1/60 s flashes of colour 2
//              (black when unset), so 0 is steady colour 1 and 255 steady colour 2
//   chase    — one param-px head per colour (default 1), evenly spaced, speed px/s
//   rainbow  — param wheel repeats (default 1), speed rotates; colours unused
//   blobs    — param soft blobs (default 3, max 16) drifting and bouncing, one
//              colour each, additive where they meet; speed ≈ px/s
//   gradient — the palette spread param times along the strip, scrolling with speed
//   fade     — whole strip crossfades through the palette; speed = rate
//   twinkle  — pixels flare up in palette colours on black; param = density
//              (default 64/255), speed = tempo
//   fire     — value-noise flames rising from pixel 0, heat mapped black →
//              colour 1 → … → colour n; speed = flicker
//   scanner  — param-px eye (default 5 % of the strip) sweeping back and forth
//              with a fading trail, next colour each pass; speed px/s
//   wave     — param brightness waves (default 2) travelling over the palette
//   stripes  — param-px bands (default 4) of each colour marching at speed px/s
namespace fx {

inline void solid(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                  uint32_t t) {
    const uint64_t pos = static_cast<uint64_t>(t) * speed * 60 % 255000;
    const bool flash   = pos < static_cast<uint64_t>(speed) * 1000;
    const Rgb c        = flash ? (p.n > 1 ? p.c[1] : Rgb{ 0, 0, 0 }) : p.c[0];
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void chase(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                  uint8_t param, uint32_t t) {
    std::memset(d, 0, static_cast<size_t>(n) * bpp);
    const uint16_t width = param ? param : 1;
    const uint32_t head  = (static_cast<uint64_t>(t) * speed / 1000) % n;
    for (uint8_t k = 0; k < p.n; ++k) {
        const uint32_t h = head + static_cast<uint32_t>(k) * n / p.n;
        for (uint16_t w = 0; w < width && w < n; ++w)
            set_px(d, bpp, static_cast<uint16_t>((h + n - w % n) % n), p.c[k]);
    }
}

inline void rainbow(uint8_t* d, uint16_t n, uint8_t bpp, uint8_t speed, uint8_t param, uint32_t t) {
    const uint32_t repeats = param ? param : 1;
    const uint32_t offset  = (static_cast<uint64_t>(t) * speed / 100) % 360;
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t hue = static_cast<uint32_t>(i) * 360 * repeats / n + offset;
        uint8_t r, g, b;
        hue_to_rgb(hue, &r, &g, &b);
        set_px(d, bpp, i, r, g, b);
    }
}

inline void blobs(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                  uint8_t param, uint32_t t) {
    std::memset(d, 0, static_cast<size_t>(n) * bpp);
    const uint32_t count = param ? (param > 16 ? 16 : param) : 3;
    const uint32_t span  = n > 1 ? n - 1u : 1u;
    uint32_t radius      = n / ((count + 1) * 2);
    if (radius < 2) radius = 2;
    const uint32_t r256 = radius * 256;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t h = hash32(k * 0x9E3779B9u + 1);
        // 0.6×..1.4× the nominal speed, so blobs overtake and cross each other.
        const uint32_t vel    = 96 + (h & 127);
        const uint64_t travel = static_cast<uint64_t>(t) * speed * vel * 256 / (160 * 1000);
        const uint32_t pos    = bounce256(travel + (h >> 8) % (2 * span * 256), span);
        const Rgb c           = p.c[k % p.n];
        const uint32_t lo     = pos > r256 ? (pos - r256) / 256 : 0;
        const uint32_t hi     = (pos + r256) / 256;
        for (uint32_t i = lo; i <= hi && i < n; ++i) {
            const uint32_t x    = i * 256;
            const uint32_t dist = x > pos ? x - pos : pos - x;
            if (dist >= r256) continue;
            add_px(d, bpp, static_cast<uint16_t>(i), scale_rgb(c, ease8(255 - dist * 255 / r256)));
        }
    }
}

inline void gradient(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                     uint8_t param, uint32_t t) {
    const uint32_t repeats = param ? param : 1;
    const uint32_t offset  = static_cast<uint32_t>(static_cast<uint64_t>(t) * speed * 256 / 1000);
    for (uint16_t i = 0; i < n; ++i)
        set_px(
            d, bpp, i,
            palette_at(p, static_cast<uint32_t>(static_cast<uint64_t>(i) * 65536u * repeats / n) -
                              offset));
}

inline void fade(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed, uint32_t t) {
    const Rgb c = palette_at(p,
                             static_cast<uint32_t>(static_cast<uint64_t>(t) * speed * 256 / 1000));
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void twinkle(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                    uint8_t param, uint32_t t) {
    const uint32_t density = param ? param : 64;
    const uint32_t ticks   = static_cast<uint32_t>(static_cast<uint64_t>(t) * (speed + 4u) / 64);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t h      = hash32(i * 2654435761u ^ 0x5bd1e995u);
        const uint32_t period = 512 + (h & 1023);
        const uint32_t u      = ticks + (h >> 12);
        const uint32_t cycle  = u / period;
        const uint32_t local  = u % period;
        const uint32_t roll   = hash32(h ^ (cycle * 0x27d4eb2du));
        if (local < 256 && (roll & 255) < density)
            set_px(d, bpp, i,
                   scale_rgb(p.c[(roll >> 8) % p.n], wave8(static_cast<uint8_t>(local))));
        else
            set_px(d, bpp, i, 0, 0, 0);
    }
}

inline void fire(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed, uint32_t t) {
    const uint32_t y    = static_cast<uint32_t>(static_cast<uint64_t>(t) * (speed + 8u) / 32);
    const uint32_t span = n > 1 ? n - 1u : 1u;
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t u    = static_cast<uint32_t>(i) * 65535u / span;  // 0 at the base
        const uint32_t fall = 255 - (u >> 8);
        // Sampling further down the strip as time runs makes the noise rise.
        const uint32_t base = u * 8 / 256 + 0x40000000u - y / 4;
        // Coarse tongues plus a finer, faster octave for the flicker.
        const uint32_t nv = (2u * noise8(base, y / 4) + noise8(base * 3, y / 2)) / 3;
        uint32_t heat     = nv * fall / 255 * 5 / 4 + fall / 5;
        if (heat > 255) heat = 255;
        set_px(d, bpp, i, palette_ramp(p, heat));
    }
}

inline void scanner(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                    uint8_t param, uint32_t t) {
    const uint32_t span   = n > 1 ? n - 1u : 1u;
    const uint32_t width  = param ? param : (n / 20 ? n / 20 : 1);
    const uint32_t half   = width * 128;
    const uint32_t trail  = width * 4 * 256;
    const uint64_t travel = static_cast<uint64_t>(t) * speed * 256 / 1000;
    const uint64_t leg    = static_cast<uint64_t>(span) * 256;
    const bool forward    = travel % (2 * leg) < leg;
    const uint32_t pos    = bounce256(travel, span);
    const Rgb c           = p.c[(travel / leg) % p.n];
    for (uint16_t i = 0; i < n; ++i) {
        const int32_t x      = static_cast<int32_t>(i) * 256;
        const int32_t behind = forward ? static_cast<int32_t>(pos) - x
                                       : x - static_cast<int32_t>(pos);
        const uint32_t ad    = static_cast<uint32_t>(behind < 0 ? -behind : behind);
        uint32_t v           = 0;
        if (ad <= half) {
            v = 255;
        } else if (behind > 0 && ad - half < trail) {
            v = 255 - (ad - half) * 255 / trail;
            v = v * v / 255;
        }
        set_px(d, bpp, i, scale_rgb(c, v));
    }
}

inline void wave(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                 uint8_t param, uint32_t t) {
    const uint32_t waves = param ? param : 2;
    const uint32_t shift = static_cast<uint32_t>(static_cast<uint64_t>(t) * speed * 512 / 1000);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t pos = static_cast<uint32_t>(static_cast<uint64_t>(i) * 65536u * waves / n) -
                             shift;
        const Rgb c = p.n > 1 ? palette_at(p, static_cast<uint32_t>(i) * 65536u / n) : p.c[0];
        set_px(d, bpp, i, scale_rgb(c, wave8(static_cast<uint8_t>(pos >> 8))));
    }
}

inline void stripes(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                    uint8_t param, uint32_t t) {
    const uint32_t width  = param ? param : 4;
    const uint32_t bands  = p.n > 1 ? p.n : 2;  // a lone colour alternates with black
    const uint32_t period = width * bands;
    const uint32_t shift  = static_cast<uint32_t>(static_cast<uint64_t>(t) * speed / 1000 % period);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t band = (i + period - shift) % period / width;
        set_px(d, bpp, i, band < p.n ? p.c[band] : Rgb{ 0, 0, 0 });
    }
}

}  // namespace fx

// Renders one frame of `scene` at wall-clock time `phase_ms` (animation speed
// is refresh-rate independent). Canonical RGB(W) order; colour order and
// brightness apply at encode time.
inline void fill_scene_pattern(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                               uint8_t bytes_per_pixel, const config::Scene& scene,
                               uint32_t phase_ms) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0 || pixel_count == 0) return;
    const Palette p = scene_palette(scene);
    const uint8_t s = scene.speed, k = scene.param;
    const uint16_t n  = pixel_count;
    const uint8_t bpp = bytes_per_pixel;
    switch (scene.effect) {
    case config::kSceneFxChase: fx::chase(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxRainbow: fx::rainbow(dst, n, bpp, s, k, phase_ms); break;
    case config::kSceneFxBlobs: fx::blobs(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxGradient: fx::gradient(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxFade: fx::fade(dst, n, bpp, p, s, phase_ms); break;
    case config::kSceneFxTwinkle: fx::twinkle(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxFire: fx::fire(dst, n, bpp, p, s, phase_ms); break;
    case config::kSceneFxScanner: fx::scanner(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxWave: fx::wave(dst, n, bpp, p, s, k, phase_ms); break;
    case config::kSceneFxStripes: fx::stripes(dst, n, bpp, p, s, k, phase_ms); break;
    default: fx::solid(dst, n, bpp, p, s, phase_ms); break;
    }
}

// ── 2-source merge (HTP/LTP) ────────────────────────────────────────────────
//
// Art-Net nodes must merge up to two concurrent senders per universe: HTP
// takes the per-slot maximum (dimmer semantics), LTP lets the last full frame
// win. A third sender is ignored, and a source that stays silent past the
// timeout is dropped. Sources are keyed by an opaque nonzero 32-bit id
// (sender IPv4 for ArtDmx, CID hash for sACN); 0 marks a free slot.
// The per-protocol timeout policy constants live in dmx_manager.h.

struct MergeState {
    uint32_t id[2];
    int64_t last_us[2];
};

inline void merge_expire(MergeState& m, int64_t now_us, int64_t timeout_us) {
    for (int i = 0; i < 2; ++i)
        if (m.id[i] != 0 && (now_us - m.last_us[i]) > timeout_us) m.id[i] = 0;
}

// Claim or refresh the slot for `id`. Returns the slot index, or -1 when both
// slots are held by other (live) sources. `*fresh` is set when the id was not
// tracked before, so the caller can clear that source's staging buffer.
inline int merge_claim(MergeState& m, uint32_t id, int64_t now_us, bool* fresh) {
    if (id == 0) id = 1;  // 0 is the free-slot sentinel
    if (fresh) *fresh = false;
    for (int i = 0; i < 2; ++i) {
        if (m.id[i] == id) {
            m.last_us[i] = now_us;
            return i;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (m.id[i] == 0) {
            m.id[i]      = id;
            m.last_us[i] = now_us;
            if (fresh) *fresh = true;
            return i;
        }
    }
    return -1;
}

inline int merge_active_count(const MergeState& m) {
    return (m.id[0] != 0 ? 1 : 0) + (m.id[1] != 0 ? 1 : 0);
}

inline void merge_drop(MergeState& m, uint32_t id) {
    if (id == 0) id = 1;
    for (int i = 0; i < 2; ++i)
        if (m.id[i] == id) m.id[i] = 0;
}

inline void htp_max(uint8_t* dst, const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        dst[i] = a[i] > b[i] ? a[i] : b[i];
}

// Ingest one network frame from `id` into a universe slot. `staging` is the
// slot's per-source store (2 × kUniverseSize), `dst` the live output buffer.
// Returns false when the frame is dropped (third concurrent source).
inline bool merge_ingest(MergeState& m, uint8_t* staging, uint8_t* dst, const uint8_t* data,
                         size_t len, uint32_t id, bool ltp, int64_t now_us, int64_t timeout_us) {
    merge_expire(m, now_us, timeout_us);
    bool fresh    = false;
    const int idx = merge_claim(m, id, now_us, &fresh);
    if (idx < 0) return false;

    if (len > kUniverseSize) len = kUniverseSize;
    uint8_t* mine = staging + static_cast<size_t>(idx) * kUniverseSize;
    // Zero a fresh claim so the previous occupant's tail can't leak into HTP.
    if (fresh) std::memset(mine, 0, kUniverseSize);
    std::memcpy(mine, data, len);

    if (!ltp && merge_active_count(m) == 2) {
        htp_max(dst, staging, staging + kUniverseSize, kUniverseSize);
    } else {
        std::memcpy(dst, data, len);  // single source, or LTP: last frame wins
    }
    return true;
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

// ── Show control: master, blackout, strobe, crossfade ───────────────────────

constexpr uint16_t kMasterFull = 65535;

// Scales `n` bytes by a 16-bit master (65535 = unchanged, 0 = dark).
inline void apply_master(uint8_t* buf, size_t n, uint16_t level) {
    if (level == kMasterFull) return;
    if (level == 0) {
        std::memset(buf, 0, n);
        return;
    }
    const uint32_t l = level;
    for (size_t i = 0; i < n; ++i)
        buf[i] = static_cast<uint8_t>((buf[i] * l + 32767u) / 65535u);
}

// Mixes `from` into `to` in place: w256 = weight of `to` (0 = all `from`,
// 256 = all `to`).
inline void blend_into(uint8_t* to, const uint8_t* from, size_t n, uint32_t w256) {
    if (w256 >= 256) return;
    const uint32_t wf = 256 - w256;
    for (size_t i = 0; i < n; ++i)
        to[i] = static_cast<uint8_t>((to[i] * w256 + from[i] * wf + 128) >> 8);
}

// Crossfade weight of the new source, 0..256, `elapsed_ms` into a `len_ms`
// fade (eased so the ends are soft).
inline uint32_t fade_weight(uint32_t elapsed_ms, uint32_t len_ms) {
    if (len_ms == 0 || elapsed_ms >= len_ms) return 256;
    const uint32_t lin = elapsed_ms * 256u / len_ms;
    return (lin * lin * (3 * 256 - 2 * lin)) / (256u * 256u);  // smoothstep
}

// Strobe rate in tenths of Hz: DMX 0 = off, 1..255 = 1..25 Hz.
constexpr uint8_t kStrobeMaxHz10 = 250;
inline uint8_t strobe_hz10_from_dmx(uint8_t v) {
    if (v == 0) return 0;
    return static_cast<uint8_t>(10 + (static_cast<uint32_t>(v) - 1) * (kStrobeMaxHz10 - 10) / 254);
}

// Whether a strobing output is lit at `now_ms`: a short flash (≤ 30 ms, at
// most half the period) at the start of every period. 0 Hz = always lit.
inline bool strobe_lit(uint32_t now_ms, uint8_t hz10) {
    if (hz10 == 0) return true;
    const uint32_t period = 10000u / hz10;
    const uint32_t on     = period / 2 < 30 ? period / 2 : 30;
    return (now_ms % period) < on;
}

// Band of 8 values: 0-7 = 0 ("none"), 8-15 = 1, ... (a fader held a little off
// its mark still selects the right item — the gobo-wheel convention).
inline uint8_t dmx_band(uint8_t v) {
    return static_cast<uint8_t>(v / 8);
}

// Effect override: 0 = none (-1), 1..255 spread evenly over the effects.
inline int effect_from_dmx(uint8_t v) {
    if (v == 0) return -1;
    return static_cast<int>((static_cast<uint32_t>(v) - 1) * config::kSceneFxCount / 255);
}

// Per-output scene overrides from the control universe (-1 = not overridden).
struct SceneOverride {
    int16_t speed  = -1;
    int16_t param  = -1;
    int16_t effect = -1;
    int16_t color[config::kSceneColorsMax][3];
    SceneOverride() {
        for (auto& c : color)
            c[0] = c[1] = c[2] = -1;
    }
};

inline void apply_scene_override(config::Scene& sc, const SceneOverride& o) {
    if (o.speed >= 0) sc.speed = static_cast<uint8_t>(o.speed);
    if (o.param >= 0) sc.param = static_cast<uint8_t>(o.param);
    if (o.effect >= 0) sc.effect = static_cast<uint8_t>(o.effect);
    for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
        const int16_t* c = o.color[k];
        if (c[0] < 0 && c[1] < 0 && c[2] < 0) continue;
        config::set_scene_color(sc, k, static_cast<uint8_t>(c[0] < 0 ? 0 : c[0]),
                                static_cast<uint8_t>(c[1] < 0 ? 0 : c[1]),
                                static_cast<uint8_t>(c[2] < 0 ? 0 : c[2]));
        if (k + 1 > config::scene_num_colors(sc)) sc.num_colors = static_cast<uint8_t>(k + 1);
    }
}

// What one control-universe frame asks for. Masters multiply, blackouts OR,
// the fastest strobe wins; per-output fields follow the slots' masks.
struct ControlEval {
    uint16_t master[config::kNumChannels];
    uint8_t blackout;  // outputs forced dark
    uint8_t strobe_hz10[config::kNumChannels];
    SceneOverride ovr[config::kNumChannels];
    int32_t fade_ms;    // -1 = no Fade slot
    int16_t fseq_band;  // -1 = no Fseq slot
    // Scene selectors in slot order: band (0 = none) + outputs.
    uint8_t n_scene;
    uint8_t scene_band[config::kMaxControlSlots];
    uint8_t scene_mask[config::kMaxControlSlots];

    ControlEval() { reset(); }
    void reset() {
        for (auto& m : master)
            m = kMasterFull;
        blackout = 0;
        std::memset(strobe_hz10, 0, sizeof(strobe_hz10));
        for (auto& o : ovr)
            o = SceneOverride{};
        fade_ms   = -1;
        fseq_band = -1;
        n_scene   = 0;
    }
};

// `dmx` is the control universe from slot 1; `len` how many slots it holds
// (a short packet leaves the rest at 0). Colour slots override a colour only
// when R, G or B is non-zero, so an idle desk leaves the scene's palette.
inline void evaluate_control(const config::ControlConfig& c, const uint8_t* dmx, size_t len,
                             ControlEval& out) {
    out.reset();
    size_t at = c.address - 1u;
    auto slot = [&](size_t i) -> uint8_t { return i < len ? dmx[i] : 0; };
    // Colour channels are gathered first, then applied per colour triple.
    int16_t col[config::kNumChannels][config::kSceneColorsMax][3];
    std::memset(col, 0xFF, sizeof(col));  // -1
    for (size_t i = 0; i < c.count && i < config::kMaxControlSlots; ++i) {
        const config::ControlSlot& s = c.slots[i];
        const uint8_t v              = slot(at);
        const uint8_t mask           = s.mask ? s.mask : 0xFF;
        const auto fn                = static_cast<config::CtlFn>(s.fn);
        for (size_t o = 0; o < config::kNumChannels; ++o) {
            if (!((mask >> o) & 1)) continue;
            switch (fn) {
            case config::CtlFn::Master: {
                uint32_t lvl  = (s.flags & config::kCtlFlagFine)
                                  ? (static_cast<uint32_t>(v) << 8) | slot(at + 1)
                                  : static_cast<uint32_t>(v) * 257u;
                out.master[o] = static_cast<uint16_t>(out.master[o] * lvl / kMasterFull);
                break;
            }
            case config::CtlFn::Blackout:
                if (v >= 128) out.blackout |= static_cast<uint8_t>(1u << o);
                break;
            case config::CtlFn::Strobe: {
                const uint8_t hz = strobe_hz10_from_dmx(v);
                if (hz > out.strobe_hz10[o]) out.strobe_hz10[o] = hz;
                break;
            }
            case config::CtlFn::Speed:
                if (v) out.ovr[o].speed = v;
                break;
            case config::CtlFn::Param:
                if (v) out.ovr[o].param = v;
                break;
            case config::CtlFn::Effect:
                if (v) out.ovr[o].effect = static_cast<int16_t>(effect_from_dmx(v));
                break;
            case config::CtlFn::Red:
            case config::CtlFn::Green:
            case config::CtlFn::Blue: {
                const size_t comp = static_cast<size_t>(fn) -
                                    static_cast<size_t>(config::CtlFn::Red);
                col[o][s.index % config::kSceneColorsMax][comp] = v;
                break;
            }
            default: break;
            }
        }
        if (fn == config::CtlFn::Scene) {
            out.scene_band[out.n_scene] = dmx_band(v);
            out.scene_mask[out.n_scene] = mask;
            ++out.n_scene;
        } else if (fn == config::CtlFn::Fade) {
            out.fade_ms = static_cast<int32_t>(v) * 100;
        } else if (fn == config::CtlFn::Fseq) {
            out.fseq_band = dmx_band(v);
        }
        at += config::control_slot_width(s);
    }
    for (size_t o = 0; o < config::kNumChannels; ++o)
        for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
            const int16_t* c3 = col[o][k];
            if (c3[0] <= 0 && c3[1] <= 0 && c3[2] <= 0) continue;  // unpatched or all zero
            for (int j = 0; j < 3; ++j)
                out.ovr[o].color[k][j] = c3[j] < 0 ? 0 : c3[j];
        }
}

}  // namespace pixfrog::dmx::logic
