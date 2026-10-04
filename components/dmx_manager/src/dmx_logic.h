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

// Universes the channel's DMX layout spans from its universe_start (see
// channel_layout below: packing and dmx_start included).
inline size_t channel_universes_used(const config::ChannelConfig& cc);

// ── Universe → slot map ─────────────────────────────────────────────────────
//
// Assign every channel's universe span a slot in the pool, and record which
// channels each slot feeds (a bit per channel: with compact patching two
// outputs can share a universe — it gets one slot, both bits). `uni_to_slot`
// must hold kMaxUniverseNumber + 1 entries and is filled with kNoSlot first;
// `slot_chans` must hold `num_slots` entries.
//
// Two ways a channel's span can fail to map, both reported through
// `out_unmapped` (number of universes that got no slot) rather than silently
// dropped: the pool runs out of slots, or the span would run past
// kMaxUniverseNumber (a channel patched near the top of the address space).
// Neither is allowed to write outside either array.
//
// Returns the number of slots used.
inline uint16_t build_universe_map(const config::ChannelConfig* chans, size_t n,
                                   uint16_t* uni_to_slot, uint8_t* slot_chans, size_t num_slots,
                                   size_t* out_unmapped) {
    for (size_t i = 0; i <= kMaxUniverseNumber; ++i)
        uni_to_slot[i] = kNoSlot;

    uint16_t slot   = 0;
    size_t unmapped = 0;
    for (size_t ch = 0; ch < n; ++ch) {
        const size_t universes_used = channel_universes_used(chans[ch]);
        for (size_t u = 0; u < universes_used; ++u) {
            const uint32_t uni = static_cast<uint32_t>(chans[ch].universe_start) + u;
            if (universe_routable(uni) && uni_to_slot[uni] != kNoSlot) {
                slot_chans[uni_to_slot[uni]] |= static_cast<uint8_t>(1u << ch);  // shared
                continue;
            }
            if (!universe_routable(uni) || slot >= num_slots) {
                unmapped++;
                continue;
            }
            uni_to_slot[uni] = slot;
            slot_chans[slot] = static_cast<uint8_t>(1u << ch);
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

// ── Live preview ────────────────────────────────────────────────────────────
//
// Shrink `count` pixels of `bpp` bytes (canonical RGB(W)) to at most `max_out`
// RGB triplets for the web dashboard: each output sample averages its bucket,
// W is folded into all three colours (clipped). Returns triplets written.
inline size_t downsample_rgb(const uint8_t* px, uint32_t count, uint8_t bpp, uint8_t* out,
                             size_t max_out) {
    if (!px || bpp < 3 || count == 0 || max_out == 0) return 0;
    const size_t n = count < max_out ? count : max_out;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = static_cast<uint32_t>(i * count / n);
        const uint32_t b = static_cast<uint32_t>((i + 1) * count / n);
        uint32_t sum[3]  = { 0, 0, 0 };
        for (uint32_t p = a; p < b; ++p) {
            const uint8_t* q = px + static_cast<size_t>(p) * bpp;
            const uint32_t w = bpp > 3 ? q[3] : 0;
            for (int k = 0; k < 3; ++k)
                sum[k] += q[k] + w > 255 ? 255 : q[k] + w;
        }
        for (int k = 0; k < 3; ++k)
            out[i * 3 + k] = static_cast<uint8_t>(sum[k] / (b - a));
    }
    return n;
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
                  uint64_t t) {
    const uint64_t pos = static_cast<uint64_t>(t) * speed * 60 % 255000;
    const bool flash   = pos < static_cast<uint64_t>(speed) * 1000;
    const Rgb c        = flash ? (p.n > 1 ? p.c[1] : Rgb{ 0, 0, 0 }) : p.c[0];
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void chase(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                  uint8_t param, uint64_t t) {
    std::memset(d, 0, static_cast<size_t>(n) * bpp);
    const uint16_t width = param ? param : 1;
    const uint32_t head  = (static_cast<uint64_t>(t) * speed / 1000) % n;
    for (uint8_t k = 0; k < p.n; ++k) {
        const uint32_t h = head + static_cast<uint32_t>(k) * n / p.n;
        for (uint16_t w = 0; w < width && w < n; ++w)
            set_px(d, bpp, static_cast<uint16_t>((h + n - w % n) % n), p.c[k]);
    }
}

inline void rainbow(uint8_t* d, uint16_t n, uint8_t bpp, uint8_t speed, uint8_t param, uint64_t t) {
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
                  uint8_t param, uint64_t t) {
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
                     uint8_t param, uint64_t t) {
    const uint32_t repeats = param ? param : 1;
    const uint32_t offset  = static_cast<uint32_t>(static_cast<uint64_t>(t) * speed * 256 / 1000);
    for (uint16_t i = 0; i < n; ++i)
        set_px(
            d, bpp, i,
            palette_at(p, static_cast<uint32_t>(static_cast<uint64_t>(i) * 65536u * repeats / n) -
                              offset));
}

inline void fade(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed, uint64_t t) {
    const Rgb c = palette_at(p,
                             static_cast<uint32_t>(static_cast<uint64_t>(t) * speed * 256 / 1000));
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void twinkle(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed,
                    uint8_t param, uint64_t t) {
    const uint32_t density = param ? param : 64;
    const uint64_t ticks   = t * (speed + 4u) / 64;  // 64-bit: no wrap in a lifetime
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t h      = hash32(i * 2654435761u ^ 0x5bd1e995u);
        const uint32_t period = 512 + (h & 1023);
        const uint64_t u      = ticks + (h >> 12);
        const uint32_t cycle  = static_cast<uint32_t>(u / period);  // only hashed
        const uint32_t local  = static_cast<uint32_t>(u % period);
        const uint32_t roll   = hash32(h ^ (cycle * 0x27d4eb2du));
        if (local < 256 && (roll & 255) < density)
            set_px(d, bpp, i,
                   scale_rgb(p.c[(roll >> 8) % p.n], wave8(static_cast<uint8_t>(local))));
        else
            set_px(d, bpp, i, 0, 0, 0);
    }
}

inline void fire(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t speed, uint64_t t) {
    // The noise coordinate wraps at 2^32 (every ~6 days at full speed): one
    // reseed of a chaotic field, invisible in flames.
    const uint32_t y    = static_cast<uint32_t>(t * (speed + 8u) / 32);
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
                    uint8_t param, uint64_t t) {
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
                 uint8_t param, uint64_t t) {
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
                    uint8_t param, uint64_t t) {
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
// is refresh-rate independent). 64-bit: a 32-bit ms clock wraps after 49.7
// days, and every effect would jump at that instant on a permanent install. Canonical RGB(W) order;
// colour order and brightness apply at encode time.
inline void fill_scene_pattern(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                               uint8_t bytes_per_pixel, const config::Scene& scene,
                               uint64_t phase_ms) {
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

// ── Fixtures ────────────────────────────────────────────────────────────────

// A run of the channel's source buffer (the pixels a scene writes).
struct Span {
    uint16_t first, count;
    bool reversed = false;  // the fixture is mounted the other way round
};

// Each fixture of `cc` as a source-buffer span, ascending: dead LEDs inside a
// fixture are skipped, invert flips the order, grouping divides it (the
// encoder maps the buffer the same way). Fixtures past the strip's end are
// cut or dropped. Returns the count.
inline size_t fixture_spans(const config::ChannelConfig& cc, Span* out, size_t cap) {
    const size_t nf       = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    const size_t ng       = led::gap_count(cc.gaps, led::kMaxPixelGaps);
    const uint32_t live_n = cc.pixel_count;
    const uint32_t group  = cc.grouping ? cc.grouping : 1;
    size_t k              = 0;
    for (size_t i = 0; i < nf && k < cap; ++i) {
        const uint32_t end = static_cast<uint32_t>(cc.fixtures[i].pos) +
                             config::fixture_len(cc.fixtures[i]);
        uint32_t a = led::live_within(cc.fixtures[i].pos, cc.gaps, ng);
        uint32_t b = led::live_within(end, cc.gaps, ng);
        if (b > live_n) b = live_n;
        if (b <= a) continue;
        if (cc.invert_direction) {
            const uint32_t t = a;
            a                = live_n - b;
            b                = live_n - t;
        }
        const uint32_t ga = a / group, gb = (b + group - 1) / group;
        out[k++] = Span{ static_cast<uint16_t>(ga), static_cast<uint16_t>(gb - ga),
                         config::fixture_reversed(cc.fixtures[i]) };
    }
    if (cc.invert_direction)
        for (size_t i = 0; i < k / 2; ++i) {
            const Span t   = out[i];
            out[i]         = out[k - 1 - i];
            out[k - 1 - i] = t;
        }
    return k;
}

// Renders `scene` on channel `cc`, spread over its fixtures as the scene's
// fixture_mode says; pixels outside every fixture stay dark. A channel
// without fixtures (or a Strip scene) gets the plain whole-strip pattern.
// A fixture mounted the other way round runs the effect backwards: its
// pixels are flipped in place once the pattern is drawn.
inline void reverse_fixtures(uint8_t* dst, uint8_t bpp, const Span* sp, size_t n) {
    uint8_t tmp[8];
    for (size_t i = 0; i < n; ++i) {
        if (!sp[i].reversed || sp[i].count < 2) continue;
        uint8_t* a = dst + static_cast<size_t>(sp[i].first) * bpp;
        uint8_t* b = a + static_cast<size_t>(sp[i].count - 1) * bpp;
        for (; a < b; a += bpp, b -= bpp) {
            std::memcpy(tmp, a, bpp);
            std::memcpy(a, b, bpp);
            std::memcpy(b, tmp, bpp);
        }
    }
}

inline void fill_scene_on_channel(uint8_t* dst, size_t dst_capacity,
                                  const config::ChannelConfig& cc, uint8_t bpp,
                                  const config::Scene& scene, uint64_t phase_ms) {
    Span sp[config::kMaxFixtures];
    const uint8_t mode = config::scene_mode_of(scene.fixture_mode);
    const size_t n     = mode == config::kFixtureModeStrip || mode >= config::kFixtureModeCount
                           ? 0
                           : fixture_spans(cc, sp, config::kMaxFixtures);
    const size_t total = static_cast<size_t>(cc.pixel_count) * bpp;
    if (n == 0 || total > dst_capacity || bpp == 0) {
        fill_scene_pattern(dst, dst_capacity, cc.pixel_count, bpp, scene, phase_ms);
        return;
    }
    if (mode == config::kFixtureModeEach) {
        std::memset(dst, 0, total);
        for (size_t i = 0; i < n; ++i)
            fill_scene_pattern(dst + static_cast<size_t>(sp[i].first) * bpp,
                               dst_capacity - static_cast<size_t>(sp[i].first) * bpp, sp[i].count,
                               bpp, scene, phase_ms);
        reverse_fixtures(dst, bpp, sp, n);
        return;
    }
    // Chain / Mirror: one pattern as long as the chained fixtures, drawn at the
    // head of the buffer, then each piece moved out to its fixture. Last first:
    // a fixture sits at or past its chained position, so nothing not yet moved
    // is overwritten.
    const size_t chained = mode == config::kFixtureModeMirror ? (n + 1) / 2 : n;
    uint32_t len         = 0;
    for (size_t i = 0; i < chained; ++i)
        len += sp[i].count;
    fill_scene_pattern(dst, dst_capacity, static_cast<uint16_t>(len), bpp, scene, phase_ms);
    uint32_t at = len;
    for (size_t i = chained; i-- > 0;) {
        at -= sp[i].count;
        std::memmove(dst + static_cast<size_t>(sp[i].first) * bpp,
                     dst + static_cast<size_t>(at) * bpp, static_cast<size_t>(sp[i].count) * bpp);
    }
    // Mirror: fixture n-1-i is fixture i reversed (resampled when the two
    // differ in length), so the look is symmetric about the middle.
    for (size_t i = chained; i < n; ++i) {
        const Span& from = sp[n - 1 - i];
        for (uint32_t j = 0; j < sp[i].count; ++j) {
            const uint32_t src = from.count - 1 - j * from.count / sp[i].count;
            std::memcpy(dst + (static_cast<size_t>(sp[i].first) + j) * bpp,
                        dst + (static_cast<size_t>(from.first) + src) * bpp, bpp);
        }
    }
    // Dark outside the fixtures.
    uint32_t cursor = 0;
    for (size_t i = 0; i <= n; ++i) {
        const uint32_t until = i < n ? sp[i].first : cc.pixel_count;
        if (until > cursor)
            std::memset(dst + static_cast<size_t>(cursor) * bpp, 0,
                        static_cast<size_t>(until - cursor) * bpp);
        if (i<n&& static_cast<uint32_t>(sp[i].first) + sp[i].count> cursor)
            cursor = static_cast<uint32_t>(sp[i].first) + sp[i].count;
    }
    reverse_fixtures(dst, bpp, sp, n);
}

// ── Scenes on groups ────────────────────────────────────────────────────────
//
// A group is an ordered list of fixtures on any outputs (config::FixtureGroup).
// A scene playing on it is drawn once along the group's "virtual strip" — the
// members end to end, in group order — then each member's slice is copied
// into its fixture on its output.

// Pixels a..a+n of a buffer, back to front.
inline void reverse_px(uint8_t* d, uint8_t bpp, uint32_t n) {
    uint8_t tmp[8];
    if (n < 2) return;
    for (uint8_t *a = d, *b = d + static_cast<size_t>(n - 1) * bpp; a < b; a += bpp, b -= bpp) {
        std::memcpy(tmp, a, bpp);
        std::memcpy(a, b, bpp);
        std::memcpy(b, tmp, bpp);
    }
}

// Each fixture of `cc` by its index (strip order), as a source-buffer span;
// count 0 for a fixture cut off by the strip's end. `flip` is set when the
// buffer runs through the fixture against its strip order (an inverted output)
// or the fixture is mounted backwards — not both.
inline void fixture_spans_by_index(const config::ChannelConfig& cc,
                                   Span out[config::kMaxFixtures]) {
    const size_t nf       = config::fixture_count(cc.fixtures, config::kMaxFixtures);
    const size_t ng       = led::gap_count(cc.gaps, led::kMaxPixelGaps);
    const uint32_t live_n = cc.pixel_count;
    const uint32_t group  = cc.grouping ? cc.grouping : 1;
    for (size_t i = 0; i < config::kMaxFixtures; ++i) {
        out[i] = Span{ 0, 0, false };
        if (i >= nf) continue;
        const uint32_t end = static_cast<uint32_t>(cc.fixtures[i].pos) +
                             config::fixture_len(cc.fixtures[i]);
        uint32_t a = led::live_within(cc.fixtures[i].pos, cc.gaps, ng);
        uint32_t b = led::live_within(end, cc.gaps, ng);
        if (b > live_n) b = live_n;
        if (b <= a) continue;
        if (cc.invert_direction) {
            const uint32_t t = a;
            a                = live_n - b;
            b                = live_n - t;
        }
        const uint32_t ga = a / group, gb = (b + group - 1) / group;
        out[i] = Span{ static_cast<uint16_t>(ga), static_cast<uint16_t>(gb - ga),
                       config::fixture_reversed(cc.fixtures[i]) != cc.invert_direction };
    }
}

// Draws `scene` along a virtual strip of members `lens[0..n)` (pixels each,
// end to end) into `strip` (RGB, 3 bytes a pixel), honouring the scene's
// fixture mode and direction:
//   each           every member plays the effect on its own
//   strip, chain   one effect over the whole strip
//   mirror         over the first half of the members, mirrored on the rest
//   reverse        the effect runs from the far end (each: from each member's)
// Returns the strip length, 0 when it does not fit `cap` bytes.
inline uint32_t render_group_strip(uint8_t* strip, size_t cap, const uint16_t* lens, size_t n,
                                   const config::Scene& scene, uint64_t phase_ms) {
    constexpr uint8_t bpp = 3;
    uint32_t total        = 0;
    for (size_t i = 0; i < n; ++i)
        total += lens[i];
    if (total == 0 || static_cast<size_t>(total) * bpp > cap) return 0;
    const uint8_t mode = config::scene_mode_of(scene.fixture_mode);
    const bool rev     = config::scene_reverse_of(scene.fixture_mode);
    if (mode == config::kFixtureModeEach) {
        uint32_t at = 0;
        for (size_t i = 0; i < n; at += lens[i++]) {
            fill_scene_pattern(strip + static_cast<size_t>(at) * bpp, cap - at * bpp, lens[i], bpp,
                               scene, phase_ms);
            if (rev) reverse_px(strip + static_cast<size_t>(at) * bpp, bpp, lens[i]);
        }
        return total;
    }
    if (mode != config::kFixtureModeMirror) {
        fill_scene_pattern(strip, cap, static_cast<uint16_t>(total), bpp, scene, phase_ms);
        if (rev) reverse_px(strip, bpp, total);
        return total;
    }
    // Mirror: the first half drawn as one strip, then member n-1-i is member
    // i back to front (resampled when the two differ in length).
    const size_t half = (n + 1) / 2;
    uint32_t hlen     = 0;
    for (size_t i = 0; i < half; ++i)
        hlen += lens[i];
    fill_scene_pattern(strip, cap, static_cast<uint16_t>(hlen), bpp, scene, phase_ms);
    if (rev) reverse_px(strip, bpp, hlen);
    uint32_t offs[config::kMaxGroupMembers + 1];
    offs[0] = 0;
    for (size_t i = 0; i < n && i < config::kMaxGroupMembers; ++i)
        offs[i + 1] = offs[i] + lens[i];
    for (size_t i = half; i < n; ++i) {
        const size_t from = n - 1 - i;
        for (uint32_t j = 0; j < lens[i]; ++j) {
            const uint32_t src = lens[from] ? lens[from] - 1 - j * lens[from] / lens[i] : 0;
            std::memcpy(strip + static_cast<size_t>(offs[i] + j) * bpp,
                        strip + static_cast<size_t>(offs[from] + src) * bpp, bpp);
        }
    }
    return total;
}

// One member's slice (`count` RGB pixels) into its fixture's span of an
// output buffer of `bpp` bytes a pixel (W off), back to front when `flip`.
inline void put_member(uint8_t* dst, uint8_t bpp, const Span& sp, const uint8_t* slice,
                       uint32_t count) {
    const uint32_t n = count < sp.count ? count : sp.count;
    for (uint32_t j = 0; j < n; ++j) {
        const uint8_t* p = slice + static_cast<size_t>(sp.reversed ? n - 1 - j : j) * 3;
        set_px(dst, bpp, static_cast<uint16_t>(sp.first + j), p[0], p[1], p[2]);
    }
}

// ── DMX layout ──────────────────────────────────────────────────────────────
//
// Where each byte of a channel's pixel buffer comes from on the wire, from
// (universe_start, dmx_start), as runs of one universe each:
//   continuous   byte after byte, a pixel may straddle two universes
//   whole        whole pixels only: a universe ends early rather than split one
//                (170 RGB / 128 RGBW from slot 1 — the "510 channels" convention)
//   per fixture  every fixture from slot 1 of a universe of its own (the first
//                one from dmx_start), whole pixels inside; pixels in no fixture
//                get no data. Without fixtures: whole.
struct DmxRun {
    uint16_t uni_off;  // from universe_start
    uint16_t slot;     // 0-based in that universe
    uint16_t dst;      // byte offset in the pixel buffer
    uint16_t bytes;
};
constexpr size_t kMaxDmxRuns = 96;

// Calls visit(run) for each run of the layout, in order (universes ascending),
// and returns how many there were. Nothing is stored: the render task, the
// boot task and the UI call this on small stacks (an on-stack run array
// overflowed app_main's 4 kB stack at boot).
template <typename Visit>
inline size_t for_each_dmx_run(const config::ChannelConfig& cc, Visit visit) {
    const uint32_t bpp = static_cast<uint32_t>(led::bytes_per_pixel(cc.protocol));
    if (bpp == 0 || cc.pixel_count == 0) return 0;
    uint32_t uni = 0, slot = cc.dmx_start > 0 ? cc.dmx_start - 1u : 0u;
    uni        += slot / kUniverseSize;  // a start past slot 512 rolls into the next universe
    slot       %= kUniverseSize;
    size_t k    = 0;
    auto place  = [&](uint32_t dst, uint32_t bytes, bool whole) {
        while (bytes) {
            const uint32_t room = kUniverseSize - slot;
            uint32_t take       = bytes < room ? bytes : room;
            if (whole) take -= take % bpp;
            if (take == 0) {  // not a whole pixel left in this universe
                ++uni;
                slot = 0;
                continue;
            }
            visit(DmxRun{ static_cast<uint16_t>(uni), static_cast<uint16_t>(slot),
                          static_cast<uint16_t>(dst), static_cast<uint16_t>(take) });
            ++k;
            dst   += take;
            bytes -= take;
            slot  += take;
            if (slot >= kUniverseSize) {
                ++uni;
                slot = 0;
            }
        }
    };
    const uint32_t total = static_cast<uint32_t>(cc.pixel_count) * bpp;
    if (cc.packing == config::kPackContinuous) {
        place(0, total, false);
        return k;
    }
    Span sp[config::kMaxFixtures];
    const size_t nf = cc.packing == config::kPackPerFixture
                        ? fixture_spans(cc, sp, config::kMaxFixtures)
                        : 0;
    if (nf == 0) {
        place(0, total, true);
    } else {
        for (size_t i = 0; i < nf; ++i) {
            if (i > 0 && slot != 0) {  // each fixture opens a universe
                ++uni;
                slot = 0;
            }
            const uint32_t from = static_cast<uint32_t>(sp[i].first) * bpp;
            if (from >= total) break;
            const uint32_t len = static_cast<uint32_t>(sp[i].count) * bpp;
            place(from, from + len > total ? total - from : len, true);
        }
    }
    return k;
}

// The runs into `out` (up to `cap`); returns how many the layout needs.
inline size_t channel_layout(const config::ChannelConfig& cc, DmxRun* out, size_t cap) {
    size_t k = 0;
    return for_each_dmx_run(cc, [&](const DmxRun& r) {
        if (k < cap) out[k] = r;
        ++k;
    });
}

// The last run (the highest universe), or false when there is none.
inline bool last_dmx_run(const config::ChannelConfig& cc, DmxRun* last) {
    return for_each_dmx_run(cc, [&](const DmxRun& r) { *last = r; }) > 0;
}

inline size_t channel_universes_used(const config::ChannelConfig& cc) {
    DmxRun last{};
    return last_dmx_run(cc, &last) ? last.uni_off + 1u : 0u;
}

// ── Auto-patch ──────────────────────────────────────────────────────────────
//
// Lays the channels out one after the other from a flat 15-bit `base`
// universe (the cursor rolls through subnet/net boundaries naturally):
//   aligned   every channel opens a universe (dmx_start 1)
//   compact   a channel starts at the slot after the previous one, sharing its
//             universe; a whole-pixel channel skips to the next universe when
//             not even one pixel fits, a per-fixture one always opens one.
// `packing` >= 0 is applied to every channel first (-1 = each keeps its own).
// A disabled / 0-pixel channel takes no room. out_uni/out_dmx receive each
// channel's start; returns the first universe nothing uses after them, and
// *slot_after (if set) the slot the last channel stopped at (0 = a fresh
// universe), for the control universe to follow in compact mode.
struct AutoPatchOptions {
    uint16_t base  = 0;
    bool compact   = false;
    int8_t packing = -1;
};

inline uint16_t compute_auto_patch(const AutoPatchOptions& o, config::ChannelConfig* chans,
                                   size_t n, uint16_t* out_uni, uint16_t* out_dmx,
                                   uint16_t* slot_after = nullptr) {
    uint32_t cur_uni = o.base, cur_slot = 0;
    for (size_t i = 0; i < n; ++i) {
        auto& c = chans[i];
        if (o.packing >= 0) c.packing = static_cast<uint8_t>(o.packing);
        const uint32_t bpp = static_cast<uint32_t>(led::bytes_per_pixel(c.protocol));
        if (bpp == 0 || c.pixel_count == 0) {  // takes no room: parked at the next free one
            out_uni[i]       = static_cast<uint16_t>((cur_slot ? cur_uni + 1 : cur_uni) & 0x7FFF);
            out_dmx[i]       = 1;
            c.universe_start = out_uni[i];
            c.dmx_start      = 1;
            continue;
        }
        const bool opens = !o.compact || c.packing == config::kPackPerFixture ||
                           (c.packing == config::kPackWholePixels &&
                            kUniverseSize - cur_slot < bpp);
        if (opens && cur_slot > 0) {
            ++cur_uni;
            cur_slot = 0;
        }
        out_uni[i]       = static_cast<uint16_t>(cur_uni & 0x7FFF);
        out_dmx[i]       = static_cast<uint16_t>(cur_slot + 1);
        c.universe_start = out_uni[i];
        c.dmx_start      = out_dmx[i];
        DmxRun last{};
        last_dmx_run(c, &last);
        cur_uni  = cur_uni + last.uni_off;
        cur_slot = static_cast<uint32_t>(last.slot) + last.bytes;
        if (cur_slot >= kUniverseSize) {
            ++cur_uni;
            cur_slot = 0;
        }
    }
    if (slot_after) *slot_after = static_cast<uint16_t>(cur_slot);
    return static_cast<uint16_t>((cur_slot ? cur_uni + 1 : cur_uni) & 0x7FFF);
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
    // Pixels no run covers (per fixture: outside every fixture) stay dark.
    if (cc.packing != config::kPackContinuous) std::memset(dst, 0, total);
    bool all = true;
    for_each_dmx_run(cc, [&](const DmxRun& r) {
        const uint8_t* src = get_universe(static_cast<uint16_t>(cc.universe_start + r.uni_off));
        if (src) {
            std::memcpy(dst + r.dst, src + r.slot, r.bytes);
        } else {
            std::memset(dst + r.dst, 0, r.bytes);
            all = false;
        }
    });
    return all;
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
inline bool strobe_lit(uint64_t now_ms, uint8_t hz10) {
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
