// dmx_logic — pure (header-only) helpers shared between dmx_manager.cpp
// and its host-side unit tests.
//
// Everything here is constexpr/inline-eligible and depends only on
// led_protocols and config_store struct definitions. No IDF symbols.

#pragma once

#include <cmath>
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

// Universes the channel takes: those of its pixel layout, from its
// universe_start (see channel_layout below: packing and dmx_start included),
// and those of its fixtures' channels, from its fixture address — the two
// ranges of an output driven both ways (config::pixel_mapped /
// fixture_controlled). channel_universes_used is their sum.
inline size_t pixel_universes_used(const config::ChannelConfig& cc,
                                   const config::ProfileBank* profiles = nullptr);
inline size_t fixture_universes_used(const config::ChannelConfig& cc,
                                     const config::ProfileBank* profiles = nullptr);
inline size_t channel_universes_used(const config::ChannelConfig& cc,
                                     const config::ProfileBank* profiles = nullptr);

// Fixture control being switched on for output `ch` (`cc`, its edited copy): a
// fixture address inside some output's pixels — never set, it is universe 0 —
// would have pixel data read as the fixtures' channels. Moves it to slot 1 of
// the first universe after everything patched; true when it moved.
// `get(i)` returns output i's stored configuration.
template <typename GetChan>
inline bool fixtures_clear_of_pixels(config::ChannelConfig& cc, size_t ch, size_t num_channels,
                                     GetChan get, const config::ProfileBank* profiles) {
    const uint32_t at = config::fix_universe(cc);
    uint32_t free     = 0;
    bool clash        = false;
    for (size_t i = 0; i < num_channels; ++i) {
        const config::ChannelConfig& o = i == ch ? cc : get(i);
        if (led::is_off(o.protocol)) continue;
        const uint32_t px = static_cast<uint32_t>(pixel_universes_used(o, profiles));
        if (px) {
            if (at >= o.universe_start && at < o.universe_start + px) clash = true;
            if (o.universe_start + px > free) free = o.universe_start + px;
        }
        const uint32_t fx = i == ch ? 0
                                    : static_cast<uint32_t>(fixture_universes_used(o, profiles));
        if (fx && config::fix_universe(o) + fx > free) free = config::fix_universe(o) + fx;
    }
    if (!clash) return false;
    config::set_fix_address(cc, static_cast<uint16_t>(free > 32767 ? 32767 : free), 1);
    return true;
}

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
                                   size_t* out_unmapped,
                                   const config::ProfileBank* profiles = nullptr) {
    for (size_t i = 0; i <= kMaxUniverseNumber; ++i)
        uni_to_slot[i] = kNoSlot;

    uint16_t slot   = 0;
    size_t unmapped = 0;
    auto map        = [&](size_t ch, uint32_t first, size_t count) {
        for (size_t u = 0; u < count; ++u) {
            const uint32_t uni = first + u;
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
    };
    for (size_t ch = 0; ch < n; ++ch) {  // its pixels' universes, then its fixtures'
        map(ch, chans[ch].universe_start, pixel_universes_used(chans[ch], profiles));
        map(ch, config::fix_universe(chans[ch]), fixture_universes_used(chans[ch], profiles));
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
// They are drawn at an FxTime: animation speed is refresh-rate independent.

// Where an effect is at one frame. A generator moves at its rate (units a
// millisecond, effect_rate) and the phaser at ph_rate; their positions are
// the integrals of those rates over time. Worked out as time × speed instead,
// every speed change would make the pattern jump: the whole past would be
// replayed at the new speed. With a speed that never changed both are the
// same, so outputs playing the same look stay in step.
struct FxTime {
    uint64_t ms;   // wall clock: twinkle and fire drift on it besides their rate
    uint64_t gen;  // the generator's position, rate × ms
    uint64_t ph;   // the phaser's, ph_rate × ms
};

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

// Colours past the configured count read as black.
inline Palette effect_palette(const config::Effect& e) {
    Palette p{};
    p.n = config::effect_num_colors(e);
    for (size_t k = 0; k < p.n; ++k)
        p.c[k] = { e.colors[k][0], e.colors[k][1], e.colors[k][2] };
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

inline void solid(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint32_t speed,
                  const FxTime& t) {
    const uint64_t pos = t.gen * 60 % 255000;
    const bool flash   = pos < static_cast<uint64_t>(speed) * 1000;
    const Rgb c        = flash ? (p.n > 1 ? p.c[1] : Rgb{ 0, 0, 0 }) : p.c[0];
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void chase(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                  const FxTime& t) {
    std::memset(d, 0, static_cast<size_t>(n) * bpp);
    const uint16_t width = param ? param : 1;
    const uint32_t head  = (t.gen / 1000) % n;
    for (uint8_t k = 0; k < p.n; ++k) {
        const uint32_t h = head + static_cast<uint32_t>(k) * n / p.n;
        for (uint16_t w = 0; w < width && w < n; ++w)
            set_px(d, bpp, static_cast<uint16_t>((h + n - w % n) % n), p.c[k]);
    }
}

inline void rainbow(uint8_t* d, uint16_t n, uint8_t bpp, uint8_t param, const FxTime& t) {
    const uint32_t repeats = param ? param : 1;
    const uint32_t offset  = (t.gen / 100) % 360;
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t hue = static_cast<uint32_t>(i) * 360 * repeats / n + offset;
        uint8_t r, g, b;
        hue_to_rgb(hue, &r, &g, &b);
        set_px(d, bpp, i, r, g, b);
    }
}

inline void blobs(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                  const FxTime& t) {
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
        const uint64_t travel = t.gen * vel * 256 / (160 * 1000);
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

inline void gradient(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                     const FxTime& t) {
    const uint32_t repeats = param ? param : 1;
    const uint32_t offset  = static_cast<uint32_t>(t.gen * 256 / 1000);
    for (uint16_t i = 0; i < n; ++i)
        set_px(
            d, bpp, i,
            palette_at(p, static_cast<uint32_t>(static_cast<uint64_t>(i) * 65536u * repeats / n) -
                              offset));
}

inline void fade(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, const FxTime& t) {
    const Rgb c = palette_at(p, static_cast<uint32_t>(t.gen * 256 / 1000));
    for (uint16_t i = 0; i < n; ++i)
        set_px(d, bpp, i, c);
}

inline void twinkle(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                    const FxTime& t) {
    const uint32_t density = param ? param : 64;
    const uint64_t ticks   = (t.gen + 4u * t.ms) / 64;  // 64-bit: no wrap in a lifetime
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

inline void fire(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, const FxTime& t) {
    // The noise coordinate wraps at 2^32 (every ~6 days at full speed): one
    // reseed of a chaotic field, invisible in flames.
    const uint32_t y    = static_cast<uint32_t>((t.gen + 8u * t.ms) / 32);
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

inline void scanner(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                    const FxTime& t) {
    const uint32_t span   = n > 1 ? n - 1u : 1u;
    const uint32_t width  = param ? param : (n / 20 ? n / 20 : 1);
    const uint32_t half   = width * 128;
    const uint32_t trail  = width * 4 * 256;
    const uint64_t travel = t.gen * 256 / 1000;
    const uint64_t leg    = static_cast<uint64_t>(span) * 256;
    const bool forward    = travel % (2 * leg) < leg;
    const uint32_t pos    = bounce256(travel, span);
    const uint32_t pass   = static_cast<uint32_t>(travel / leg);
    const Rgb c           = p.c[pass % p.n];
    const Rgb prev        = p.c[(pass + p.n - 1) % p.n];  // the pass before: its trail
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t x    = static_cast<uint32_t>(i) * 256;
        const uint32_t dist = x > pos ? x - pos : pos - x;
        uint32_t v          = 0;
        Rgb col             = c;
        if (dist <= half) {
            v = 255;
        } else {
            // The trail fades with the path the eye has run since it passed
            // the pixel, so it follows the eye through a turn instead of
            // vanishing there: a pixel ahead of the eye was passed on the
            // way back, in the colour of that pass. Nothing before the first.
            uint32_t behind = half + trail;  // dark
            if (forward ? x <= pos : x >= pos) {
                behind = dist;
            } else if (pass > 0) {
                behind = forward
                           ? pos + x
                           : (static_cast<uint32_t>(leg) - pos) + (static_cast<uint32_t>(leg) - x);
                col    = prev;
            }
            if (behind - half < trail) {
                v = 255 - (behind - half) * 255 / trail;
                v = v * v / 255;
            }
        }
        set_px(d, bpp, i, scale_rgb(col, v));
    }
}

inline void wave(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                 const FxTime& t) {
    const uint32_t waves = param ? param : 2;
    const uint32_t shift = static_cast<uint32_t>(t.gen * 512 / 1000);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t pos = static_cast<uint32_t>(static_cast<uint64_t>(i) * 65536u * waves / n) -
                             shift;
        const Rgb c = p.n > 1 ? palette_at(p, static_cast<uint32_t>(i) * 65536u / n) : p.c[0];
        set_px(d, bpp, i, scale_rgb(c, wave8(static_cast<uint8_t>(pos >> 8))));
    }
}

inline void stripes(uint8_t* d, uint16_t n, uint8_t bpp, const Palette& p, uint8_t param,
                    const FxTime& t) {
    const uint32_t width  = param ? param : 4;
    const uint32_t bands  = p.n > 1 ? p.n : 2;  // a lone colour alternates with black
    const uint32_t period = width * bands;
    const uint32_t shift  = static_cast<uint32_t>(t.gen / 1000 % period);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t band = (i + period - shift) % period / width;
        set_px(d, bpp, i, band < p.n ? p.c[band] : Rgb{ 0, 0, 0 });
    }
}

}  // namespace fx

// Renders one frame of `generator` (kSceneFx*) on a run of `pixel_count`
// pixels at `t`, `rate` being the speed in the generator's own units (solid
// reads it: its strobe's flash length). 64-bit positions: a 32-bit ms clock
// wraps after 49.7 days, and every effect would jump at that instant on a
// permanent install. Canonical RGB(W) order; colour order
// and brightness apply at encode time.
inline void fill_generator(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                           uint8_t bytes_per_pixel, uint8_t generator, const Palette& p,
                           uint32_t rate, uint8_t param, const FxTime& t) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0 || pixel_count == 0) return;
    const uint8_t k   = param;
    const uint16_t n  = pixel_count;
    const uint8_t bpp = bytes_per_pixel;
    switch (generator) {
    case config::kSceneFxChase: fx::chase(dst, n, bpp, p, k, t); break;
    case config::kSceneFxRainbow: fx::rainbow(dst, n, bpp, k, t); break;
    case config::kSceneFxBlobs: fx::blobs(dst, n, bpp, p, k, t); break;
    case config::kSceneFxGradient: fx::gradient(dst, n, bpp, p, k, t); break;
    case config::kSceneFxFade: fx::fade(dst, n, bpp, p, t); break;
    case config::kSceneFxTwinkle: fx::twinkle(dst, n, bpp, p, k, t); break;
    case config::kSceneFxFire: fx::fire(dst, n, bpp, p, t); break;
    case config::kSceneFxScanner: fx::scanner(dst, n, bpp, p, k, t); break;
    case config::kSceneFxWave: fx::wave(dst, n, bpp, p, k, t); break;
    case config::kSceneFxStripes: fx::stripes(dst, n, bpp, p, k, t); break;
    default: fx::solid(dst, n, bpp, p, rate, t); break;
    }
}

// The same at wall-clock `phase_ms`, the rate as it is.
inline void fill_generator(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                           uint8_t bytes_per_pixel, uint8_t generator, const Palette& p,
                           uint32_t rate, uint8_t param, uint64_t phase_ms) {
    fill_generator(dst, dst_capacity, pixel_count, bytes_per_pixel, generator, p, rate, param,
                   FxTime{ phase_ms, phase_ms * rate, 0 });
}

// The generator rate of an effect: Effect::speed counts double (twice the top
// speed of the generators' 0..255 units, half the resolution). Solid keeps its
// own scale — its speed is a strobe frequency, 255 meaning steady colour 2.
inline uint32_t effect_rate(const config::Effect& e) {
    return e.generator == config::kSceneFxSolid ? e.speed : 2u * e.speed;
}

// `e` at wall-clock `ms` with its speeds as they are: a look nobody rides.
inline FxTime wall_time(const config::Effect& e, uint64_t ms) {
    return FxTime{ ms, ms * effect_rate(e), ms * e.ph_rate };
}

// The positions of what one place draws (an output, a group, a fixture),
// kept from frame to frame so a speed change carries on from where the look
// is. `key` names the look drawn there: another one — or the same one again
// after kFxClockIdleMs undrawn, a scene stopped and restarted — starts on the
// wall clock, in step with every other place playing it.
struct FxClock {
    uint64_t gen    = 0;
    uint64_t ph     = 0;
    uint64_t at     = 0;  // ms of the last tick
    uint32_t key    = 0;
    uint16_t rate   = 0;
    uint8_t ph_rate = 0;
    bool live       = false;
};

// Advances `c` to `ms` at the rates it ran at since its last tick, then takes
// `e`'s: the frame after a change already moves at the new speed.
inline constexpr uint64_t kFxClockIdleMs = 1000;  // longer than any frame
inline FxTime clock_tick(FxClock& c, uint32_t key, const config::Effect& e, uint64_t ms) {
    if (!c.live || c.key != key || ms < c.at || ms - c.at > kFxClockIdleMs) {
        c.gen = ms * effect_rate(e);
        c.ph  = ms * e.ph_rate;
    } else {
        c.gen += (ms - c.at) * c.rate;
        c.ph  += (ms - c.at) * c.ph_rate;
    }
    c.at      = ms;
    c.key     = key;
    c.rate    = static_cast<uint16_t>(effect_rate(e));
    c.ph_rate = e.ph_rate;
    c.live    = true;
    return FxTime{ ms, c.gen, c.ph };
}

// Of the `n` clocks of one place, the one already on `key`, else the one
// ticked longest ago (a look no longer drawn there).
inline FxClock& clock_slot(FxClock* c, size_t n, uint32_t key) {
    FxClock* oldest = &c[0];
    for (size_t i = 0; i < n; ++i) {
        if (c[i].live && c[i].key == key) return c[i];
        if (c[i].at < oldest->at) oldest = &c[i];
    }
    return *oldest;
}

// ── Look transitions ────────────────────────────────────────────────────────
// A discrete pick of the look — the Effect channel of a fixture, the desk's
// Effect / Generator channels on an output or a group — crossfades from the
// old look to the new over a time the desk sends, instead of cutting (0 =
// cut, as a desk's "snap"). While that time is above 0 a new pick waits
// kLookSettleFrames unchanged frames: a fader ridden through the bands would
// otherwise start a fade at every band it crosses. A look is an opaque id,
// equal for equal picks.
constexpr uint8_t kLookSettleFrames = 3;

struct LookFade {
    int32_t shown   = 0;  // the look drawn (the fade's target)
    int32_t from    = 0;  // the look it fades from
    int32_t pending = 0;  // a pick waiting to settle
    uint32_t start  = 0;  // ms
    uint32_t last   = 0;  // ms of the last tick: one step a frame
    uint16_t len    = 0;  // ms, 0 = no fade running
    uint8_t settle  = 0;
    bool init       = false;
};

// Feeds this frame's pick `look`, with the fade time the desk asks for;
// returns the look to draw. Ticking twice in a frame (same `now`) counts once.
inline int32_t look_tick(LookFade& f, int32_t look, uint16_t fade_ms, uint32_t now) {
    if (!f.init) {
        f         = LookFade{};
        f.init    = true;
        f.shown   = look;
        f.pending = look;
        f.last    = now;
        return look;
    }
    const bool step = now != f.last;
    f.last          = now;
    if (f.len && now - f.start >= f.len) f.len = 0;
    if (look == f.shown) {
        f.pending = look;
        f.settle  = 0;
        return look;
    }
    if (fade_ms == 0) {  // a cut
        f.shown = f.pending = look;
        f.len               = 0;
        f.settle            = 0;
        return look;
    }
    if (look != f.pending) {
        f.pending = look;
        f.settle  = 1;
    } else if (step && f.settle < kLookSettleFrames) {
        ++f.settle;
    }
    if (f.settle < kLookSettleFrames) return f.shown;
    f.from   = f.shown;
    f.shown  = look;
    f.start  = now;
    f.len    = fade_ms;
    f.settle = 0;
    return look;
}

inline bool look_fading(const LookFade& f, uint32_t now) {
    return f.init && f.len && now - f.start < f.len;
}

// ── Dimmer phaser ───────────────────────────────────────────────────────────
// A dimmer layer over whatever the generator drew: a waveform travelling
// along the run (Effect::ph_*), as on a desk's phaser.

// One sine period, 0..255 around 127.5, by 1/256 of a turn.
inline constexpr uint8_t kSin8[256] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162, 165, 167, 170, 173, 176, 179, 182,
    185, 188, 190, 193, 196, 198, 201, 203, 206, 208, 211, 213, 215, 218, 220, 222, 224, 226, 228,
    230, 232, 234, 235, 237, 238, 240, 241, 243, 244, 245, 246, 248, 249, 250, 250, 251, 252, 253,
    253, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 254, 254, 254, 253, 253, 252, 251, 250,
    250, 249, 248, 246, 245, 244, 243, 241, 240, 238, 237, 235, 234, 232, 230, 228, 226, 224, 222,
    220, 218, 215, 213, 211, 208, 206, 203, 201, 198, 196, 193, 190, 188, 185, 182, 179, 176, 173,
    170, 167, 165, 162, 158, 155, 152, 149, 146, 143, 140, 137, 134, 131, 128, 124, 121, 118, 115,
    112, 109, 106, 103, 100, 97,  93,  90,  88,  85,  82,  79,  76,  73,  70,  67,  65,  62,  59,
    57,  54,  52,  49,  47,  44,  42,  40,  37,  35,  33,  31,  29,  27,  25,  23,  21,  20,  18,
    17,  15,  14,  12,  11,  10,  9,   7,   6,   5,   5,   4,   3,   2,   2,   1,   1,   1,   0,
    0,   0,   0,   0,   0,   0,   1,   1,   1,   2,   2,   3,   4,   5,   5,   6,   7,   9,   10,
    11,  12,  14,  15,  17,  18,  20,  21,  23,  25,  27,  29,  31,  33,  35,  37,  40,  42,  44,
    47,  49,  52,  54,  57,  59,  62,  65,  67,  70,  73,  76,  79,  82,  85,  88,  90,  93,  97,
    100, 103, 106, 109, 112, 115, 118, 121, 124,
};

// The waveform's level (0..255) at `phase` (one cycle per 65536). `width` is
// the share of the cycle the wave takes, n/255 (0 = all of it): the wave runs
// compressed, then holds its end level. For PWM it is the lit share (0 = half),
// and `attack` / `decay` are the shares of that lit part spent fading in and
// out (n/255 each): a square at 0, a trapezoid, a triangle when they take it
// all — past that they split it in their proportion.
inline uint8_t phaser_level(uint8_t wave, uint32_t phase, uint8_t width, uint8_t attack = 0,
                            uint8_t decay = 0) {
    uint32_t x = phase & 0xFFFF;
    if (wave == config::kPhaserPwm) {
        const uint32_t w = width ? width : 128u;
        if (x * 255 >= w * 65536) return 0;
        const uint32_t lit = w * 65536 / 255;
        uint32_t up = lit * attack / 255, down = lit * decay / 255;
        if (up + down > lit) {
            up   = lit * attack / (static_cast<uint32_t>(attack) + decay);
            down = lit - up;
        }
        if (up && x < up) return static_cast<uint8_t>(x * 255 / up);
        if (down && x + down >= lit)
            return static_cast<uint8_t>((lit > x ? lit - x : 0) * 255 / down);
        return 255;
    }
    if (width && width < 255) {
        x = x * 255 / width;
        if (x > 0xFFFF) x = 0xFFFF;
    }
    switch (wave) {
    case config::kPhaserSin: return kSin8[x >> 8];
    case config::kPhaserCos: return kSin8[((x >> 8) + 64) & 255];
    case config::kPhaserRampUp: return static_cast<uint8_t>(x >> 8);
    case config::kPhaserRampDown: return static_cast<uint8_t>(255 - (x >> 8));
    case config::kPhaserTriangle:
        return static_cast<uint8_t>(x < 0x8000 ? x >> 7 : (0xFFFF - x) >> 7);
    case config::kPhaserBump: {  // the upper half of the sine: a hump from 0 and back
        const int up = 2 * kSin8[x >> 9] - 255;
        return static_cast<uint8_t>(up > 0 ? up : 0);
    }
    default: return 255;
    }
}

// The phaser's dimmer (0..255) for pixel `i` of a run of `n` at `t`.
// ph_rate counts 1/20 Hz (3 BPM a step), ph_spread 1/16 cycle along the run.
inline uint8_t phaser_dimmer(const config::Effect& e, uint32_t i, uint32_t n, const FxTime& t) {
    const uint32_t time  = static_cast<uint32_t>(t.ph * 4096 / 1250);
    const uint32_t along = n ? i * e.ph_spread * 4096u / n : 0;
    const uint32_t phase = (e.flags & config::kEffectPhaserReverse) ? time + along : time - along;
    const uint32_t level = phaser_level(e.ph_wave, phase, e.ph_width, e.ph_attack, e.ph_decay);
    return static_cast<uint8_t>(e.ph_low + (255u - e.ph_low) * level / 255);
}

inline uint8_t phaser_dimmer(const config::Effect& e, uint32_t i, uint32_t n, uint64_t phase_ms) {
    return phaser_dimmer(e, i, n, wall_time(e, phase_ms));
}

// Applies the effect's dimmer layers to the `n` pixels the generator just
// drew: the phaser, then the invert — an intensity negative measured against
// colour 1 (a pixel as bright as colour 1 goes dark, a dark one takes colour
// 1). The invert is worked out from the generator's pixel, so a pixel the
// phaser dimmed to black still knows its hue.
inline void apply_effect_dimmer(uint8_t* d, uint16_t n, uint8_t bpp, const config::Effect& e,
                                const FxTime& t) {
    const bool phaser = e.ph_wave != config::kPhaserNone && e.ph_wave < config::kPhaserWaveCount;
    const bool invert = (e.flags & config::kEffectDimmerInvert) != 0;
    if (!phaser && !invert) return;
    const uint8_t* c1 = e.colors[0];
    uint32_t ref      = c1[0] > c1[1] ? c1[0] : c1[1];
    if (c1[2] > ref) ref = c1[2];
    // Rainbow ignores the palette: its reference is full brightness.
    if (e.generator == config::kSceneFxRainbow) ref = 255;
    for (uint16_t i = 0; i < n; ++i) {
        uint8_t* px        = d + static_cast<size_t>(i) * bpp;
        const uint32_t dim = phaser ? phaser_dimmer(e, i, n, t) + 1u : 256u;
        if (!invert) {
            for (uint8_t k = 0; k < 3; ++k)
                px[k] = static_cast<uint8_t>(px[k] * dim >> 8);
            continue;
        }
        uint32_t own = px[0] > px[1] ? px[0] : px[1];  // the generator's intensity here
        if (px[2] > own) own = px[2];
        if (own == 0) {
            px[0] = c1[0];
            px[1] = c1[1];
            px[2] = c1[2];
            continue;
        }
        const uint32_t lum  = own * dim >> 8;
        const uint32_t left = lum < ref ? ref - lum : 0;
        for (uint8_t k = 0; k < 3; ++k)
            px[k] = static_cast<uint8_t>(px[k] * left / own);
    }
}

// ── Block / Groups / Wings ──────────────────────────────────────────────────
// A desk's MAtricks, on the pixels of a run. The effect is drawn on a shorter
// virtual run, then spread out:
//   wings  — the run splits in that many parts, every other one mirrored;
//   block  — that many neighbouring pixels share one value;
//   groups — the pattern repeats every that many values.
// 0 or 1 turns each off.
struct Matricks {
    uint32_t wing;   // pixels per wing
    uint32_t block;  // pixels per value
    uint32_t virt;   // values drawn: the virtual run
    bool active;     // false: the run is drawn as is
};

inline Matricks matricks_for(const config::Effect& e, uint32_t n) {
    Matricks m{};
    const uint32_t wings = e.wings >= 2 ? (e.wings < n ? e.wings : n) : 1;
    m.wing               = wings ? (n + wings - 1) / wings : n;
    m.block              = e.block >= 2 ? e.block : 1;
    const uint32_t cells = m.block ? (m.wing + m.block - 1) / m.block : m.wing;
    m.virt               = e.groups >= 2 && e.groups < cells ? e.groups : cells;
    m.active             = n > 0 && m.virt < n;
    return m;
}

// Spreads the `m.virt` values at the head of `d` over the `n` pixels of the
// run, in place. Last pixel first: a pixel's value sits at or before it, so
// nothing still to be read is overwritten — no scratch buffer. The value
// index is kept by counters, one division per wing rather than per pixel.
inline void expand_matricks(uint8_t* d, uint32_t n, uint8_t bpp, const Matricks& m) {
    if (!m.active) return;
    const uint32_t wings = (n + m.wing - 1) / m.wing;
    for (uint32_t p = wings; p-- > 0;) {
        const uint32_t first = p * m.wing;
        const uint32_t len   = n - first < m.wing ? n - first : m.wing;
        const bool mirrored  = (p & 1) != 0;
        // Position inside the wing of the pixel being written: counted down
        // on a plain wing, up on a mirrored one.
        const uint32_t at = mirrored ? m.wing - len : len - 1;
        uint32_t in_block = at % m.block;
        uint32_t value    = at / m.block % m.virt;
        for (uint32_t j = len; j-- > 0;) {
            const uint32_t i = first + j;
            if (i != value)
                std::memcpy(d + static_cast<size_t>(i) * bpp, d + static_cast<size_t>(value) * bpp,
                            bpp);
            if (mirrored) {
                if (++in_block == m.block) {
                    in_block = 0;
                    value    = value + 1 == m.virt ? 0 : value + 1;
                }
            } else if (in_block-- == 0) {
                in_block = m.block - 1;
                value    = value == 0 ? m.virt - 1 : value - 1;
            }
        }
    }
}

// Renders one frame of `effect` on a run of `pixel_count` pixels at `t`: the
// generator and the dimmer layers on the virtual run, then Block / Groups /
// Wings spread it over the pixels.
inline void fill_effect_run(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                            uint8_t bytes_per_pixel, const config::Effect& effect,
                            const FxTime& t) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0 || pixel_count == 0) return;
    const Matricks m = matricks_for(effect, pixel_count);
    const auto drawn = static_cast<uint16_t>(m.active ? m.virt : pixel_count);
    fill_generator(dst, dst_capacity, drawn, bytes_per_pixel, effect.generator,
                   effect_palette(effect), effect_rate(effect), effect.param, t);
    apply_effect_dimmer(dst, drawn, bytes_per_pixel, effect, t);
    expand_matricks(dst, pixel_count, bytes_per_pixel, m);
}

// The same at wall-clock `phase_ms`, its speeds as they are.
inline void fill_effect_run(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                            uint8_t bytes_per_pixel, const config::Effect& effect,
                            uint64_t phase_ms) {
    fill_effect_run(dst, dst_capacity, pixel_count, bytes_per_pixel, effect,
                    wall_time(effect, phase_ms));
}

// ── Fixtures ────────────────────────────────────────────────────────────────

// A run of the channel's source buffer (the pixels a scene writes).
struct Span {
    uint16_t first, count;
    bool reversed   = false;  // the fixture is mounted the other way round
    uint8_t profile = 0;      // its DMX profile (control mode)
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
                         config::fixture_reversed(cc.fixtures[i]),
                         config::fixture_profile(cc.fixtures[i]) };
    }
    if (cc.invert_direction)
        for (size_t i = 0; i < k / 2; ++i) {
            const Span t   = out[i];
            out[i]         = out[k - 1 - i];
            out[k - 1 - i] = t;
        }
    return k;
}

// Draws on channel `cc` with `fill_run(dst, capacity, pixels)`, spread over
// its fixtures as `mode` (kFixtureMode*) says; pixels outside every fixture
// stay dark. A channel
// without fixtures (or the Strip mode) gets the plain whole-strip pattern.
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

// `mode` is a part's fixture-mode byte: the mode, and the direction bit — the
// pattern then runs from the far end (each: of every fixture; chain, mirror:
// of the chained run).
template <typename FillRun>
inline void fill_on_channel(uint8_t* dst, size_t dst_capacity, const config::ChannelConfig& cc,
                            uint8_t bpp, uint8_t mode, FillRun fill_run) {
    Span sp[config::kMaxFixtures];
    const bool rev     = config::scene_reverse_of(mode);
    mode               = config::scene_mode_of(mode);
    const size_t n     = mode == config::kFixtureModeStrip || mode >= config::kFixtureModeCount
                           ? 0
                           : fixture_spans(cc, sp, config::kMaxFixtures);
    const size_t total = static_cast<size_t>(cc.pixel_count) * bpp;
    if (n == 0 || total > dst_capacity || bpp == 0) {
        fill_run(dst, dst_capacity, cc.pixel_count);
        if (rev && bpp && total <= dst_capacity) reverse_px(dst, bpp, cc.pixel_count);
        return;
    }
    if (mode == config::kFixtureModeEach) {
        std::memset(dst, 0, total);
        for (size_t i = 0; i < n; ++i) {
            uint8_t* at = dst + static_cast<size_t>(sp[i].first) * bpp;
            fill_run(at, dst_capacity - static_cast<size_t>(sp[i].first) * bpp, sp[i].count);
            if (rev) reverse_px(at, bpp, sp[i].count);
        }
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
    fill_run(dst, dst_capacity, static_cast<uint16_t>(len));
    if (rev) reverse_px(dst, bpp, len);
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

// Renders `effect` on channel `cc`, spread over its fixtures as `mode` says.
inline void fill_effect_on_channel(uint8_t* dst, size_t dst_capacity,
                                   const config::ChannelConfig& cc, uint8_t bpp,
                                   const config::Effect& effect, uint8_t mode, const FxTime& t) {
    fill_on_channel(dst, dst_capacity, cc, bpp, mode, [&](uint8_t* d, size_t cap, uint16_t n) {
        fill_effect_run(d, cap, n, bpp, effect, t);
    });
}

inline void fill_effect_on_channel(uint8_t* dst, size_t dst_capacity,
                                   const config::ChannelConfig& cc, uint8_t bpp,
                                   const config::Effect& effect, uint8_t mode, uint64_t phase_ms) {
    fill_effect_on_channel(dst, dst_capacity, cc, bpp, effect, mode, wall_time(effect, phase_ms));
}

// ── Scenes on groups ────────────────────────────────────────────────────────
//
// A group is an ordered list of fixtures on any outputs (config::FixtureGroup).
// A scene playing on it is drawn once along the group's "virtual strip" — the
// members end to end, in group order — then each member's slice is copied
// into its fixture on its output.

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
        out[i] = Span{ 0, 0, false, 0 };
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
                       config::fixture_reversed(cc.fixtures[i]) != cc.invert_direction,
                       config::fixture_profile(cc.fixtures[i]) };
    }
}

// Draws `effect` along a virtual strip of members `lens[0..n)` (pixels each,
// end to end) into `strip` (RGB, 3 bytes a pixel), honouring the fixture mode
// and direction packed in `fixture_mode` (config::scene_mode_of / _reverse_of):
//   each           every member plays the effect on its own
//   strip, chain   one effect over the whole strip
//   mirror         over the first half of the members, mirrored on the rest
//   reverse        the effect runs from the far end (each: from each member's)
// Returns the strip length, 0 when it does not fit `cap` bytes.
inline uint32_t render_group_strip(uint8_t* strip, size_t cap, const uint16_t* lens, size_t n,
                                   const config::Effect& effect, uint8_t fixture_mode,
                                   const FxTime& t) {
    constexpr uint8_t bpp = 3;
    uint32_t total        = 0;
    for (size_t i = 0; i < n; ++i)
        total += lens[i];
    if (total == 0 || static_cast<size_t>(total) * bpp > cap) return 0;
    const uint8_t mode = config::scene_mode_of(fixture_mode);
    const bool rev     = config::scene_reverse_of(fixture_mode);
    if (mode == config::kFixtureModeEach) {
        uint32_t at = 0;
        for (size_t i = 0; i < n; at += lens[i++]) {
            fill_effect_run(strip + static_cast<size_t>(at) * bpp, cap - at * bpp, lens[i], bpp,
                            effect, t);
            if (rev) reverse_px(strip + static_cast<size_t>(at) * bpp, bpp, lens[i]);
        }
        return total;
    }
    if (mode != config::kFixtureModeMirror) {
        fill_effect_run(strip, cap, static_cast<uint16_t>(total), bpp, effect, t);
        if (rev) reverse_px(strip, bpp, total);
        return total;
    }
    // Mirror: the first half drawn as one strip, then member n-1-i is member
    // i back to front (resampled when the two differ in length).
    const size_t half = (n + 1) / 2;
    uint32_t hlen     = 0;
    for (size_t i = 0; i < half; ++i)
        hlen += lens[i];
    fill_effect_run(strip, cap, static_cast<uint16_t>(hlen), bpp, effect, t);
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

inline uint32_t render_group_strip(uint8_t* strip, size_t cap, const uint16_t* lens, size_t n,
                                   const config::Effect& effect, uint8_t fixture_mode,
                                   uint64_t phase_ms) {
    return render_group_strip(strip, cap, lens, n, effect, fixture_mode,
                              wall_time(effect, phase_ms));
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
    uint16_t fill = 0;  // > 1: these bytes are one pixel, repeated over `fill` pixels
};
constexpr size_t kMaxDmxRuns = 96;

// Calls visit(run) for each run of the layout, in order (universes ascending),
// and returns how many there were. Nothing is stored: the render task, the
// boot task and the UI call this on small stacks (an on-stack run array
// overflowed app_main's 4 kB stack at boot).
// A fixture of a control-mode channel and where its DMX channels sit.
struct FixturePatch {
    uint16_t uni_off;  // universe, relative to config::fix_universe(cc)
    uint16_t slot;     // first channel there, 0-based
    uint16_t first;    // its span of the source buffer
    uint16_t count;
    uint16_t footprint;  // channels it takes: its profile's
    uint8_t profile;     // index in the bank (clamped: a profile that is gone reads as the first)
    bool reversed;
};

// The profile a fixture uses: an index past the bank falls back on the first.
inline uint8_t patched_profile(const config::ProfileBank& bank, uint8_t index) {
    return index < bank.count ? index : uint8_t{ 0 };
}

// Calls visit(patch) for each fixture of a channel as fixture control lays
// them out, in the order they are listed (the wiring order, whatever the
// strip's direction), and returns how many there were. The fixtures follow
// each other from the fixture address (config::fix_dmx_start);
// one that would straddle the end of a universe starts the next instead. A
// channel without fixtures is one fixture covering the strip, on the first
// profile. Nothing is stored (see for_each_dmx_run).
template <typename Visit>
inline size_t for_each_fixture_patch(const config::ChannelConfig& cc,
                                     const config::ProfileBank& bank, Visit visit) {
    if (led::bytes_per_pixel(cc.protocol) == 0 || cc.pixel_count == 0 || bank.count == 0) return 0;
    Span sp[config::kMaxFixtures];
    size_t n = fixture_spans(cc, sp, config::kMaxFixtures);
    if (n == 0) {
        const uint32_t group = cc.grouping ? cc.grouping : 1;
        sp[0] = Span{ 0, static_cast<uint16_t>((cc.pixel_count + group - 1) / group), false, 0 };
        n     = 1;
    }
    uint32_t uni = 0, slot = config::fix_dmx_start(cc) > 0 ? config::fix_dmx_start(cc) - 1u : 0u;
    uni  += slot / kUniverseSize;
    slot %= kUniverseSize;
    for (size_t k = 0; k < n; ++k) {
        // fixture_spans lists them in buffer order, which invert flips.
        const Span& f         = sp[cc.invert_direction ? n - 1 - k : k];
        const uint8_t profile = patched_profile(bank, f.profile);
        const uint32_t foot   = static_cast<uint32_t>(
            config::profile_footprint(bank.profiles[profile]));
        if (slot + foot > kUniverseSize) {
            ++uni;
            slot = 0;
        }
        visit(FixturePatch{ static_cast<uint16_t>(uni), static_cast<uint16_t>(slot), f.first,
                            f.count, static_cast<uint16_t>(foot), profile, f.reversed });
        slot += foot;
        if (slot >= kUniverseSize) {
            ++uni;
            slot = 0;
        }
    }
    return n;
}

// The runs of the pixel layout; none for an output that is not pixel-mapped.
// A raw legacy "control" layout (config::legacy_control — never stored:
// sanitize_channel converts it) still reads the old way: one run per fixture,
// its channels, `dst` its rank, laid out with `profiles`.
template <typename Visit>
inline size_t for_each_dmx_run(const config::ChannelConfig& cc, Visit visit,
                               const config::ProfileBank* profiles = nullptr) {
    const uint32_t bpp = static_cast<uint32_t>(led::bytes_per_pixel(cc.protocol));
    if (bpp == 0 || cc.pixel_count == 0) return 0;
    if (config::legacy_control(cc)) {
        if (!profiles) return 0;
        uint16_t rank = 0;
        return for_each_fixture_patch(cc, *profiles, [&](const FixturePatch& f) {
            visit(DmxRun{ f.uni_off, f.slot, rank++, f.footprint });
        });
    }
    if (!config::pixel_mapped(cc)) return 0;
    const uint8_t layout = config::pixel_layout(cc);
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
    if (layout == config::kPackContinuous) {
        place(0, total, false);
        return k;
    }
    if (layout == config::kPackFixtureColour) {
        // One colour per fixture, in strip order, one pixel's channels each,
        // never split across universes; no fixtures = one colour for all.
        Span by[config::kMaxFixtures];
        fixture_spans_by_index(cc, by);
        const size_t nf = config::fixture_count(cc.fixtures, config::kMaxFixtures);
        auto one        = [&](uint32_t first, uint32_t count) {
            if (kUniverseSize - slot < bpp) {
                ++uni;
                slot = 0;
            }
            DmxRun r{ static_cast<uint16_t>(uni), static_cast<uint16_t>(slot),
                      static_cast<uint16_t>(first * bpp), static_cast<uint16_t>(bpp) };
            r.fill = static_cast<uint16_t>(count);
            visit(r);
            ++k;
            slot += bpp;
        };
        if (nf == 0) one(0, cc.pixel_count);
        for (size_t i = 0; i < nf; ++i)
            if (by[i].count) one(by[i].first, by[i].count);
        return k;
    }
    Span sp[config::kMaxFixtures];
    const size_t nf = layout == config::kPackPerFixture
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
inline size_t channel_layout(const config::ChannelConfig& cc, DmxRun* out, size_t cap,
                             const config::ProfileBank* profiles = nullptr) {
    size_t k = 0;
    return for_each_dmx_run(
        cc,
        [&](const DmxRun& r) {
            if (k < cap) out[k] = r;
            ++k;
        },
        profiles);
}

// The last run (the highest universe), or false when there is none.
inline bool last_dmx_run(const config::ChannelConfig& cc, DmxRun* last,
                         const config::ProfileBank* profiles = nullptr) {
    return for_each_dmx_run(cc, [&](const DmxRun& r) { *last = r; }, profiles) > 0;
}

inline size_t pixel_universes_used(const config::ChannelConfig& cc,
                                   const config::ProfileBank* profiles) {
    DmxRun last{};
    return last_dmx_run(cc, &last, profiles) ? last.uni_off + 1u : 0u;
}

inline size_t fixture_universes_used(const config::ChannelConfig& cc,
                                     const config::ProfileBank* profiles) {
    // A raw legacy layout is counted with the pixel runs (for_each_dmx_run).
    if (!profiles || !config::fixture_controlled(cc) || config::legacy_control(cc)) return 0;
    uint16_t last  = 0;
    const size_t n = for_each_fixture_patch(cc, *profiles,
                                            [&](const FixturePatch& f) { last = f.uni_off; });
    return n ? last + 1u : 0u;
}

inline size_t channel_universes_used(const config::ChannelConfig& cc,
                                     const config::ProfileBank* profiles) {
    return pixel_universes_used(cc, profiles) + fixture_universes_used(cc, profiles);
}

// ── Auto-patch ──────────────────────────────────────────────────────────────
//
// Lays everything out from a flat 15-bit `base` universe (the cursor rolls
// through subnet/net boundaries naturally), in blocks that each open a
// universe, ordered by how often they grow — each block only ever pushes the
// ones after it:
//   1. the control universe, when `control`: one universe, at `base`, never
//      moves;
//   2. the fixtures of the outputs under fixture control — the desk's patch:
//      aligned, every output's fixtures open a universe; compact, they follow
//      the previous output's. A fixture never straddles two universes. From
//      `fix_base` when given (a universe of their own), else right after the
//      control universe;
//   3. the pixel-mapped outputs — the media server's, the block that grows
//      with every LED added — one after the other:
//        aligned   every output opens a universe (dmx_start 1)
//        compact   an output starts at the slot after the previous one,
//                  sharing its universe; a whole-pixel output skips to the
//                  next universe when not even one pixel fits, a per-fixture
//                  one always opens one.
//      `packing` >= 0 is applied to each of them first (-1 = each keeps its own).
// A disabled / 0-pixel output takes no room. out_uni/out_dmx receive each
// output's address — its pixels', or its fixtures' when it is not pixel-mapped
// (its own address then mirrors that one); an output driven neither way is
// parked at the first free universe. *control_uni (if set) receives the
// control universe's. Returns the first universe nothing uses after the
// blocks; *slot_after (if set) is 0: whatever comes next opens a universe;
// *universes (if set) how many the blocks take.
struct AutoPatchOptions {
    uint16_t base    = 0;
    bool compact     = false;
    int8_t packing   = -1;
    int32_t fix_base = -1;
    bool control     = false;
};

inline uint16_t compute_auto_patch(const AutoPatchOptions& o, config::ChannelConfig* chans,
                                   size_t n, uint16_t* out_uni, uint16_t* out_dmx,
                                   uint16_t* slot_after                = nullptr,
                                   const config::ProfileBank* profiles = nullptr,
                                   size_t* universes = nullptr, uint16_t* control_uni = nullptr) {
    auto lit = [](const config::ChannelConfig& c) {
        return led::bytes_per_pixel(c.protocol) != 0 && c.pixel_count != 0;
    };
    for (size_t i = 0; i < n; ++i) {
        auto& c = chans[i];
        if (config::legacy_control(c)) {  // a raw old value: the two switches
            config::set_fix_address(c, c.universe_start, c.dmx_start);
            c.packing = config::kPackContinuous | config::kChanNoPixelMap | config::kChanFixtureCtl;
        }
    }
    // Block 1: the control universe.
    uint32_t cur = o.base;
    if (o.control) {
        if (control_uni) *control_uni = static_cast<uint16_t>(cur & 0x7FFF);
        ++cur;
    }
    size_t used = o.control ? 1 : 0;
    // Block 2: the fixtures.
    const uint32_t fix_first = o.fix_base >= 0 ? static_cast<uint32_t>(o.fix_base) : cur;
    uint32_t fix_uni = fix_first, fix_slot = 0;
    bool any = false;
    for (size_t i = 0; i < n && profiles; ++i) {
        auto& c = chans[i];
        if (!lit(c) || !config::fixture_controlled(c)) continue;
        // An output's first fixture would itself move to the next universe if
        // it did not fit: start the output there, so its address says where
        // its first fixture is.
        uint32_t first_foot = 0;
        for_each_fixture_patch(c, *profiles, [&](const FixturePatch& f) {
            if (!first_foot) first_foot = f.footprint;
        });
        if (fix_slot > 0 && (!o.compact || kUniverseSize - fix_slot < first_foot)) {
            ++fix_uni;
            fix_slot = 0;
        }
        config::set_fix_address(c, static_cast<uint16_t>(fix_uni & 0x7FFF),
                                static_cast<uint16_t>(fix_slot + 1));
        FixturePatch last{};
        for_each_fixture_patch(c, *profiles, [&](const FixturePatch& f) { last = f; });
        fix_uni  += last.uni_off;
        fix_slot  = static_cast<uint32_t>(last.slot) + last.footprint;
        if (fix_slot >= kUniverseSize) {
            ++fix_uni;
            fix_slot = 0;
        }
        any = true;
    }
    if (any) {
        const uint32_t fix_end  = fix_slot ? fix_uni + 1 : fix_uni;
        used                   += fix_end - fix_first;
        if (o.fix_base < 0) cur = fix_end;  // in order: the pixels follow
    }
    // Block 3: the pixels.
    const uint32_t pix_first = cur;
    uint32_t cur_slot        = 0;
    for (size_t i = 0; i < n; ++i) {
        auto& c           = chans[i];
        const bool pixels = config::pixel_mapped(c);
        // The layout asked for is a pixel layout: nothing to lay out on an
        // output that is not pixel-mapped.
        if (o.packing >= 0 && pixels) config::set_pixel_layout(c, static_cast<uint8_t>(o.packing));
        const uint32_t bpp = static_cast<uint32_t>(led::bytes_per_pixel(c.protocol));
        if (!lit(c) || !pixels) {
            if (lit(c) && config::fixture_controlled(c)) {  // its address is its fixtures'
                out_uni[i] = c.universe_start = config::fix_universe(c);
                out_dmx[i] = c.dmx_start = config::fix_dmx_start(c);
            } else {  // takes no room: parked at the next free one
                out_uni[i]       = static_cast<uint16_t>((cur_slot ? cur + 1 : cur) & 0x7FFF);
                out_dmx[i]       = 1;
                c.universe_start = out_uni[i];
                c.dmx_start      = 1;
            }
            continue;
        }
        const uint8_t layout = config::pixel_layout(c);
        const bool opens     = !o.compact || layout == config::kPackPerFixture ||
                           (layout == config::kPackWholePixels && kUniverseSize - cur_slot < bpp);
        if (opens && cur_slot > 0) {
            ++cur;
            cur_slot = 0;
        }
        out_uni[i]       = static_cast<uint16_t>(cur & 0x7FFF);
        out_dmx[i]       = static_cast<uint16_t>(cur_slot + 1);
        c.universe_start = out_uni[i];
        c.dmx_start      = out_dmx[i];
        DmxRun last{};
        last_dmx_run(c, &last, profiles);
        cur      = cur + last.uni_off;
        cur_slot = static_cast<uint32_t>(last.slot) + last.bytes;
        if (cur_slot >= kUniverseSize) {
            ++cur;
            cur_slot = 0;
        }
    }
    uint32_t end  = cur_slot ? cur + 1 : cur;
    used         += end - pix_first;
    if (any && o.fix_base >= 0) {  // the fixtures' own block may lie past the pixels
        const uint32_t fix_end = fix_slot ? fix_uni + 1 : fix_uni;
        if (fix_end > end) end = fix_end;
    }
    if (universes) *universes = used;
    if (slot_after) *slot_after = 0;
    return static_cast<uint16_t>(end & 0x7FFF);
}

// ── Patch overlap check ─────────────────────────────────────────────────────
//
// Where every range of the patch really sits — each output's pixel runs (its
// layout and start address), its fixtures' channels (their profiles), the
// control universe's — and which of them share DMX channels. Two ranges on
// the same channels both read the same data: an output that mirrors another,
// or a patch mistake. Ranges are laid on one axis, universe × 512 + slot, so
// a range that runs on into the next universe stays one interval.

// Who owns a range: output o's pixels are o, its fixtures kNumChannels + o,
// the control universe kPatchControl.
constexpr uint8_t kPatchControl = 2 * config::kNumChannels;
inline bool patch_owner_is_fixtures(uint8_t owner) {
    return owner >= config::kNumChannels && owner < kPatchControl;
}
inline uint8_t patch_owner_output(uint8_t owner) {
    return static_cast<uint8_t>(owner % config::kNumChannels);
}

struct PatchRange {
    uint32_t first, end;  // universe × 512 + slot, end excluded
    uint8_t owner;
};

// One pair of owners on the same channels: the first channel they share and
// how many they share in all.
struct PatchClash {
    uint8_t a, b;       // owners, a < b
    uint16_t universe;  // the first shared channel
    uint16_t slot;      // 0-based
    uint16_t channels;
};

// Ranges per output at most: its pixel runs and its fixtures.
constexpr size_t kMaxPatchRanges = config::kNumChannels * (kMaxDmxRuns + config::kMaxFixtures) + 1;

// Every pair of owners at most: what find_patch_clashes may report.
constexpr size_t kMaxPatchClashes = (kPatchControl + 1u) * kPatchControl / 2u;

// Lists the patch's ranges into `out` (up to `cap`): runs of one owner that
// follow each other are joined. `get_chan(o)` gives output o's settings.
// Returns how many there are.
template <typename GetChan>
inline size_t collect_patch_ranges(GetChan get_chan, size_t n, const config::ProfileBank& profiles,
                                   const config::ControlConfig& control, PatchRange* out,
                                   size_t cap) {
    size_t k = 0;
    auto add = [&](uint8_t owner, uint32_t first, uint32_t len) {
        if (len == 0) return;
        if (k > 0 && out[k - 1].owner == owner && out[k - 1].end == first) {
            out[k - 1].end += len;
            return;
        }
        if (k < cap) out[k++] = PatchRange{ first, first + len, owner };
    };
    for (size_t o = 0; o < n && o < config::kNumChannels; ++o) {
        const config::ChannelConfig& cc = get_chan(o);
        if (cc.protocol == led::Protocol::Off) continue;
        const auto px = static_cast<uint8_t>(o);
        const auto fx = static_cast<uint8_t>(config::kNumChannels + o);
        for_each_dmx_run(cc, [&](const DmxRun& r) {
            add(px, (static_cast<uint32_t>(cc.universe_start) + r.uni_off) * kUniverseSize + r.slot,
                r.bytes);
        });
        if (config::fixture_controlled(cc))
            for_each_fixture_patch(cc, profiles, [&](const FixturePatch& f) {
                add(fx,
                    (static_cast<uint32_t>(config::fix_universe(cc)) + f.uni_off) * kUniverseSize +
                        f.slot,
                    f.footprint);
            });
    }
    if (control.enabled) {
        const uint32_t at = control.address > 0 ? control.address - 1u : 0u;
        add(kPatchControl, static_cast<uint32_t>(control.universe) * kUniverseSize + at,
            static_cast<uint32_t>(config::control_footprint(control)));
    }
    return k;
}

// The clashes among `r[0..n)` (sorted in place) into `out`, one per pair of
// owners, up to `cap` (kMaxPatchClashes holds them all). Returns how many
// were written.
inline size_t find_patch_clashes(PatchRange* r, size_t n, PatchClash* out, size_t cap) {
    for (size_t i = 1; i < n; ++i) {  // insertion sort: the list is mostly in order already
        const PatchRange v = r[i];
        size_t j           = i;
        for (; j > 0 && r[j - 1].first > v.first; --j)
            r[j] = r[j - 1];
        r[j] = v;
    }
    size_t pairs = 0;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n && r[j].first < r[i].end; ++j) {
            if (r[j].owner == r[i].owner) continue;
            const uint32_t from = r[j].first;
            const uint32_t to   = r[j].end < r[i].end ? r[j].end : r[i].end;
            const uint8_t a     = r[i].owner < r[j].owner ? r[i].owner : r[j].owner;
            const uint8_t b     = r[i].owner < r[j].owner ? r[j].owner : r[i].owner;
            size_t c            = 0;
            while (c < pairs && !(out[c].a == a && out[c].b == b))
                ++c;
            if (c < pairs) {
                // Already met: the first shared channel stays the lowest.
                const uint32_t at = static_cast<uint32_t>(out[c].universe) * kUniverseSize +
                                    out[c].slot;
                if (from < at) {
                    out[c].universe = static_cast<uint16_t>(from / kUniverseSize);
                    out[c].slot     = static_cast<uint16_t>(from % kUniverseSize);
                }
                const uint32_t sum = out[c].channels + (to - from);
                out[c].channels    = static_cast<uint16_t>(sum < 0xFFFF ? sum : 0xFFFF);
                continue;
            }
            if (pairs < cap)
                out[pairs++] = PatchClash{ a, b, static_cast<uint16_t>(from / kUniverseSize),
                                           static_cast<uint16_t>(from % kUniverseSize),
                                           static_cast<uint16_t>(to - from) };
        }
    return pairs;
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
    if (config::pixel_layout(cc) != config::kPackContinuous) std::memset(dst, 0, total);
    bool all = true;
    for_each_dmx_run(cc, [&](const DmxRun& r) {
        const uint8_t* src = get_universe(static_cast<uint16_t>(cc.universe_start + r.uni_off));
        if (src && r.fill > 1) {  // one colour over a whole fixture
            for (uint32_t p = 0; p < r.fill && r.dst + (p + 1) * r.bytes <= total; ++p)
                std::memcpy(dst + r.dst + p * r.bytes, src + r.slot, r.bytes);
        } else if (src) {
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

// The manual strobe (control universe Strobe, a profile's Strobe), in tenths
// of Hz: 0-9 = none — the dead band of the LED fixtures' strobe channels, so a
// fader resting near 0 does not flicker — then 10-255 = 1..25 Hz.
constexpr uint8_t kStrobeMaxHz10  = 250;
constexpr uint8_t kStrobeDeadBand = config::kStrobeDeadBand;
inline uint8_t strobe_hz10_from_dmx(uint8_t v) {
    if (v < kStrobeDeadBand) return 0;
    return static_cast<uint8_t>(10 + (static_cast<uint32_t>(v) - kStrobeDeadBand) *
                                         (kStrobeMaxHz10 - 10) / (255 - kStrobeDeadBand));
}

// Whether a strobing output is lit at `now_ms`: a short flash (≤ 30 ms, at
// most half the period) at the start of every period. 0 Hz = always lit.
inline bool strobe_lit(uint64_t now_ms, uint8_t hz10) {
    if (hz10 == 0) return true;
    const uint32_t period = 10000u / hz10;
    const uint32_t on     = period / 2 < 30 ? period / 2 : 30;
    return (now_ms % period) < on;
}

// ── A fixture's shutter ─────────────────────────────────────────────────────
// What lets its light out, frame by frame, over whatever it draws. The pro
// Shutter channel follows the ladder of the pixel bars that have one (Elation
// SixBar, Ayrton MagicBlade): bands of 32, every effect between two "open"
// bands so a fader passing from one to the next shows light, and closed at 0.
// Rates rise with the value in each band.
enum : uint8_t {
    kShutterOpen = 0,
    kShutterClosed,
    kShutterStrobe,  // a short flash a period (strobe_lit)
    kShutterPulse,   // a smooth swell and fall a period
    kShutterRandom,  // a flash at a random moment of each period, each fixture its own
};
struct ShutterState {
    uint8_t kind = kShutterOpen;
    uint8_t hz10 = 0;  // the rate, tenths of Hz
};

inline uint8_t band_rate(uint8_t v, uint8_t lo_hz10, uint8_t hi_hz10) {  // v within a band of 32
    return static_cast<uint8_t>(lo_hz10 + (v % 32u) * (hi_hz10 - lo_hz10) / 31u);
}

// The pro Shutter channel: 0-31 closed · 32-63 open · 64-95 strobe 1-25 Hz ·
// 96-127 open · 128-159 pulse 0.5-10 Hz · 160-191 open · 192-223 random strobe
// 1-20 flashes a second · 224-255 open.
inline ShutterState shutter_from_dmx(uint8_t v) {
    switch (v / 32) {
    case 0: return ShutterState{ kShutterClosed, 0 };
    case 2: return ShutterState{ kShutterStrobe, band_rate(v, 10, kStrobeMaxHz10) };
    case 4: return ShutterState{ kShutterPulse, band_rate(v, 5, 100) };
    case 6: return ShutterState{ kShutterRandom, band_rate(v, 10, 200) };
    default: return ShutterState{};  // open
    }
}

// The manual Strobe channel as a shutter state.
inline ShutterState strobe_from_dmx(uint8_t v) {
    const uint8_t hz10 = strobe_hz10_from_dmx(v);
    return hz10 ? ShutterState{ kShutterStrobe, hz10 } : ShutterState{};
}

// How much light the shutter lets out at `now_ms`, 0..255. `seed` tells the
// fixtures apart for the random strobe.
inline uint8_t shutter_level(const ShutterState& s, uint64_t now_ms, uint32_t seed) {
    switch (s.kind) {
    case kShutterClosed: return 0;
    case kShutterStrobe: return strobe_lit(now_ms, s.hz10) ? 255 : 0;
    case kShutterPulse: {
        if (s.hz10 == 0) return 255;
        const uint32_t period = 10000u / s.hz10;
        const uint32_t x      = static_cast<uint32_t>(now_ms % period) * 256u / period;
        return kSin8[(x + 192u) & 255u];  // a raised cosine: dark, full, dark
    }
    case kShutterRandom: {
        if (s.hz10 == 0) return 255;
        // One flash per period, at a moment drawn for that period and fixture.
        const uint32_t period = 10000u / s.hz10;
        const uint32_t on     = period / 2 < 30 ? period / 2 : 30;
        const uint64_t slot   = now_ms / period;
        const uint32_t at     = hash32(static_cast<uint32_t>(slot) * 0x9E3779B9u ^ seed) %
                            (period - on + 1);
        const uint32_t t = static_cast<uint32_t>(now_ms % period);
        return t >= at && t < at + on ? 255 : 0;
    }
    default: return 255;
    }
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
// What a desk asks of the effect a scene plays; -1 = its own.
struct EffectOverride {
    int16_t bank      = -1;  // another effect of the bank altogether
    int16_t speed     = -1;
    int16_t param     = -1;
    int16_t generator = -1;
    int16_t ph_wave   = -1;
    int16_t ph_rate   = -1;
    int16_t ph_spread = -1;
    int16_t ph_width  = -1;
    int16_t ph_attack = -1;
    int16_t ph_decay  = -1;
    int16_t block     = -1;
    int16_t groups    = -1;
    int16_t wings     = -1;
    int8_t reverse    = -1;  // 0 forward, 1 from the far end
    int8_t fix_mode   = -1;  // config::kFixtureMode*
    int16_t color[config::kSceneColorsMax][3];
    EffectOverride() {
        for (auto& c : color)
            c[0] = c[1] = c[2] = -1;
    }
};

// A part's fixture-mode byte with the desk's Direction / Fixture mode on it.
inline uint8_t apply_mode_override(uint8_t fixture_mode, const EffectOverride& o) {
    if (o.reverse < 0 && o.fix_mode < 0) return fixture_mode;
    return config::pack_scene_mode(
        o.fix_mode >= 0 ? static_cast<uint8_t>(o.fix_mode) : config::scene_mode_of(fixture_mode),
        o.reverse >= 0 ? o.reverse == 1 : config::scene_reverse_of(fixture_mode), -1);
}

// The Speed channel is a tempo over the look as a whole — the generator's
// motion and the dimmer phaser's rate, whatever the effect is made of — not
// a value of one of them: 128 = as stored, down to ÷10 at 1, up to ×10 at
// 255, log scale. A rate at 0 (no motion) stays 0; a Solid's 255 (steady
// colour 2, not a strobe) stays 255.
inline uint8_t tempo_scaled(uint8_t rate, uint8_t tempo, bool solid) {
    if (rate == 0 || tempo == 128 || (solid && rate == 255)) return rate;
    const float f = std::exp2(static_cast<float>(static_cast<int>(tempo) - 128) *
                              (3.3219f / 127.f));
    const int v   = static_cast<int>(static_cast<float>(rate) * f + 0.5f);
    return static_cast<uint8_t>(v < 1 ? 1 : v > 255 ? 255 : v);
}

// Everything but `bank` and the fixture mode: swapping the effect itself is
// the caller's, which holds the bank (see dmx_manager's render_source).
inline void apply_effect_override(config::Effect& e, const EffectOverride& o) {
    auto take = [](uint8_t& field, int16_t v) {
        if (v >= 0) field = static_cast<uint8_t>(v);
    };
    if (o.speed > 0) {  // the tempo first: an absolute phaser rate below wins
        e.speed   = tempo_scaled(e.speed, static_cast<uint8_t>(o.speed),
                                 e.generator == config::kSceneFxSolid);
        e.ph_rate = tempo_scaled(e.ph_rate, static_cast<uint8_t>(o.speed), false);
    }
    take(e.param, o.param);
    take(e.generator, o.generator);
    take(e.ph_wave, o.ph_wave);
    take(e.ph_rate, o.ph_rate);
    take(e.ph_spread, o.ph_spread);
    take(e.ph_width, o.ph_width);
    take(e.ph_attack, o.ph_attack);
    take(e.ph_decay, o.ph_decay);
    take(e.block, o.block);
    take(e.groups, o.groups);
    take(e.wings, o.wings);
    for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
        const int16_t* c = o.color[k];
        if (c[0] < 0 && c[1] < 0 && c[2] < 0) continue;
        for (size_t j = 0; j < 3; ++j)
            e.colors[k][j] = static_cast<uint8_t>(c[j] < 0 ? 0 : c[j]);
        if (k + 1 > config::effect_num_colors(e)) e.num_colors = static_cast<uint8_t>(k + 1);
    }
}

// An Attack / Decay channel: 0 is "the effect's own" (the caller's), 1 hard
// edges, then up to the whole lit part at 255.
inline int16_t envelope_from_dmx(uint8_t v) {
    return v <= 1 ? 0 : v;
}

// Phaser wave a desk's wave channel selects, in bands of 8: -1 = the effect's
// own (band 0, or past the last wave), else kPhaser* — band 1 is "no phaser".
inline int phaser_wave_from_dmx(uint8_t v) {
    const uint8_t band = dmx_band(v);
    return band >= 1 && band <= config::kPhaserWaveCount ? band - 1 : -1;
}

// What one control-universe frame asks for. Masters multiply, blackouts OR,
// the fastest strobe wins; per-output fields follow the slots' masks.
struct ControlEval {
    uint16_t master[config::kNumChannels];
    uint8_t blackout;  // outputs forced dark
    uint8_t strobe_hz10[config::kNumChannels];
    EffectOverride ovr[config::kNumChannels];
    int32_t fade_ms;    // -1 = no Fade slot
    int16_t fseq_band;  // -1 = no Fseq slot
    // Scene selectors in slot order: band (0 = none) + outputs, or a group.
    uint8_t n_scene;
    uint8_t scene_band[config::kMaxControlSlots];
    uint8_t scene_mask[config::kMaxControlSlots];
    int8_t scene_group[config::kMaxControlSlots];  // -1 = on scene_mask
    // Group-targeted slots: what they ask of the scenes playing on each group,
    // and a master / blackout over its fixtures.
    EffectOverride govr[config::kMaxGroups];
    uint16_t gmaster[config::kMaxGroups];
    uint32_t gblackout;  // bit per group

    ControlEval() { reset(); }
    void reset() {
        for (auto& m : master)
            m = kMasterFull;
        blackout = 0;
        std::memset(strobe_hz10, 0, sizeof(strobe_hz10));
        for (auto& o : ovr)
            o = EffectOverride{};
        fade_ms   = -1;
        fseq_band = -1;
        n_scene   = 0;
        for (auto& o : govr)
            o = EffectOverride{};
        for (auto& m : gmaster)
            m = kMasterFull;
        gblackout = 0;
    }
};

// A FixMode channel's value: 0 = the scene's own (-1), then four bands.
inline int fix_mode_from_dmx(uint8_t v) {
    if (v == 0) return -1;
    if (v < 64) return config::kFixtureModeEach;
    if (v < 128) return config::kFixtureModeChain;
    if (v < 192) return config::kFixtureModeMirror;
    return config::kFixtureModeStrip;
}

// One group-targeted slot (its value `v`, `v2` the next channel for a fine
// Master) into the group's fields of `out`; colours gathered in `gcol`.
inline void apply_group_slot(ControlEval& out, int16_t (*gcol)[config::kSceneColorsMax][3],
                             int group, config::CtlFn fn, const config::ControlSlot& s, uint8_t v,
                             uint8_t v2) {
    if (group < 0 || group >= static_cast<int>(config::kMaxGroups)) return;
    EffectOverride& o = out.govr[group];
    switch (fn) {
    case config::CtlFn::Master: {
        const uint32_t lvl = (s.flags & config::kCtlFlagFine) ? (static_cast<uint32_t>(v) << 8) | v2
                                                              : static_cast<uint32_t>(v) * 257u;
        out.gmaster[group] = static_cast<uint16_t>(out.gmaster[group] * lvl / kMasterFull);
        break;
    }
    case config::CtlFn::Blackout:
        if (v >= 128) out.gblackout |= 1u << group;
        break;
    case config::CtlFn::Speed:
        if (v) o.speed = v;
        break;
    case config::CtlFn::Param:
        if (v) o.param = v;
        break;
    case config::CtlFn::Effect:
        if (v) o.generator = static_cast<int16_t>(effect_from_dmx(v));
        break;
    case config::CtlFn::Bank:
        if (dmx_band(v)) o.bank = static_cast<int16_t>(dmx_band(v) - 1);
        break;
    case config::CtlFn::PhWave: o.ph_wave = static_cast<int16_t>(phaser_wave_from_dmx(v)); break;
    case config::CtlFn::PhRate:
        if (v) o.ph_rate = v;
        break;
    case config::CtlFn::PhSpread:
        if (v) o.ph_spread = v;
        break;
    case config::CtlFn::PhWidth:
        if (v) o.ph_width = v;
        break;
    case config::CtlFn::PhAttack:
        if (v) o.ph_attack = envelope_from_dmx(v);
        break;
    case config::CtlFn::PhDecay:
        if (v) o.ph_decay = envelope_from_dmx(v);
        break;
    case config::CtlFn::Block:
        if (v) o.block = v;
        break;
    case config::CtlFn::Groups:
        if (v) o.groups = v;
        break;
    case config::CtlFn::Wings:
        if (v) o.wings = v;
        break;
    case config::CtlFn::Direction:
        if (v) o.reverse = v >= 128 ? 1 : 0;
        break;
    case config::CtlFn::FixMode:
        if (v) o.fix_mode = static_cast<int8_t>(fix_mode_from_dmx(v));
        break;
    case config::CtlFn::Red:
    case config::CtlFn::Green:
    case config::CtlFn::Blue:
        gcol[group][s.index % config::kSceneColorsMax]
            [static_cast<size_t>(fn) - static_cast<size_t>(config::CtlFn::Red)] = v;
        break;
    default: break;
    }
}

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
    int16_t gcol[config::kMaxGroups][config::kSceneColorsMax][3];
    std::memset(gcol, 0xFF, sizeof(gcol));
    for (size_t i = 0; i < c.count && i < config::kMaxControlSlots; ++i) {
        const config::ControlSlot& s = c.slots[i];
        const uint8_t v              = slot(at);
        const int group              = config::control_slot_group(s);
        const uint8_t mask           = group >= 0 ? 0 : (s.mask ? s.mask : 0xFF);
        const auto fn                = static_cast<config::CtlFn>(s.fn);
        if (group >= 0) apply_group_slot(out, gcol, group, fn, s, v, slot(at + 1));
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
                if (v) out.ovr[o].generator = static_cast<int16_t>(effect_from_dmx(v));
                break;
            case config::CtlFn::Bank:
                if (dmx_band(v)) out.ovr[o].bank = static_cast<int16_t>(dmx_band(v) - 1);
                break;
            case config::CtlFn::PhWave:
                out.ovr[o].ph_wave = static_cast<int16_t>(phaser_wave_from_dmx(v));
                break;
            case config::CtlFn::PhRate:
                if (v) out.ovr[o].ph_rate = v;
                break;
            case config::CtlFn::PhSpread:
                if (v) out.ovr[o].ph_spread = v;
                break;
            case config::CtlFn::PhWidth:
                if (v) out.ovr[o].ph_width = v;
                break;
            case config::CtlFn::PhAttack:
                if (v) out.ovr[o].ph_attack = envelope_from_dmx(v);
                break;
            case config::CtlFn::PhDecay:
                if (v) out.ovr[o].ph_decay = envelope_from_dmx(v);
                break;
            case config::CtlFn::Block:
                if (v) out.ovr[o].block = v;
                break;
            case config::CtlFn::Groups:
                if (v) out.ovr[o].groups = v;
                break;
            case config::CtlFn::Wings:
                if (v) out.ovr[o].wings = v;
                break;
            case config::CtlFn::Direction:
                if (v) out.ovr[o].reverse = v >= 128 ? 1 : 0;
                break;
            case config::CtlFn::FixMode:
                if (v) out.ovr[o].fix_mode = static_cast<int8_t>(fix_mode_from_dmx(v));
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
            out.scene_band[out.n_scene]  = dmx_band(v);
            out.scene_mask[out.n_scene]  = mask;
            out.scene_group[out.n_scene] = static_cast<int8_t>(group);
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
    for (size_t g = 0; g < config::kMaxGroups; ++g)
        for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
            const int16_t* c3 = gcol[g][k];
            if (c3[0] <= 0 && c3[1] <= 0 && c3[2] <= 0) continue;
            for (int j = 0; j < 3; ++j)
                out.govr[g].color[k][j] = c3[j] < 0 ? 0 : c3[j];
        }
}

// ── DMX control mode ────────────────────────────────────────────────────────
// An output in control mode is driven like conventional luminaires: each of
// its fixtures reads the channels of its DMX profile and plays what they ask
// for — a colour, or an effect of the bank with the desk's overrides.

// What one fixture's channels ask for.
// What the Effect channel picks, beside an effect of the bank (0..):
//   kBankNone   (0)    the fixture's own light — colour 1, steady; over pixel
//                      mapping, its pixels as they come;
//   kBankColour (1-7)  colour 1, steady, in both cases: the desk's plain RGB
//                      fixture, pixels or not.
constexpr int16_t kBankNone   = -1;
constexpr int16_t kBankColour = -2;

struct FixtureFrame {
    uint16_t dimmer = kMasterFull;  // full unless the profile has a dimmer
    uint8_t white   = 0;
    ShutterState shutter;                       // the pro Shutter channel (open without one)
    ShutterState strobe;                        // the manual Strobe channel
    int16_t bank        = kBankNone;            // kBankNone, kBankColour or an effect
    uint16_t fx_fade_ms = 0;                    // a change of `bank` crossfades this long
    int16_t color[config::kSceneColorsMax][3];  // -1 = not in the profile
    EffectOverride fx;                          // speed, param, phaser, MAtricks
    FixtureFrame() { std::memset(color, 0xFF, sizeof(color)); }
};

// Reads `dmx` — the fixture's channels, profile_footprint(p) of them — as the
// profile says.
inline void decode_fixture(const config::Profile& p, const uint8_t* dmx, FixtureFrame& out) {
    out       = FixtureFrame{};
    size_t at = 0;
    for (size_t i = 0; i < p.count && i < config::kMaxProfileSlots; ++i) {
        const config::ProfileSlot& s = p.slots[i];
        const uint8_t v              = dmx[at];
        const auto fn                = static_cast<config::FixFn>(s.fn);
        switch (fn) {
        case config::FixFn::Dimmer:
            out.dimmer = (s.arg & config::kProfileArgFine)
                           ? static_cast<uint16_t>((v << 8) | dmx[at + 1])
                           : static_cast<uint16_t>(v * 257u);
            break;
        case config::FixFn::Red:
        case config::FixFn::Green:
        case config::FixFn::Blue:
            out.color[s.arg & config::kProfileArgColor]
                     [static_cast<size_t>(fn) - static_cast<size_t>(config::FixFn::Red)] = v;
            break;
        case config::FixFn::White: out.white = v; break;
        case config::FixFn::Shutter: out.shutter = shutter_from_dmx(v); break;
        case config::FixFn::Strobe: out.strobe = strobe_from_dmx(v); break;
        case config::FixFn::Bank:
            if (dmx_band(v))
                out.bank = static_cast<int16_t>(dmx_band(v) - 1);
            else if (v)
                out.bank = kBankColour;
            break;
        case config::FixFn::Speed:
            if (v) out.fx.speed = v;
            break;
        case config::FixFn::Param:
            if (v) out.fx.param = v;
            break;
        case config::FixFn::PhWave:
            out.fx.ph_wave = static_cast<int16_t>(phaser_wave_from_dmx(v));
            break;
        case config::FixFn::PhRate:
            if (v) out.fx.ph_rate = v;
            break;
        case config::FixFn::PhSpread:
            if (v) out.fx.ph_spread = v;
            break;
        case config::FixFn::PhWidth:
            if (v) out.fx.ph_width = v;
            break;
        case config::FixFn::PhAttack:
            if (v) out.fx.ph_attack = envelope_from_dmx(v);
            break;
        case config::FixFn::PhDecay:
            if (v) out.fx.ph_decay = envelope_from_dmx(v);
            break;
        case config::FixFn::FxFade: out.fx_fade_ms = static_cast<uint16_t>(v * 100u); break;
        case config::FixFn::Block:
            if (v) out.fx.block = v;
            break;
        case config::FixFn::Groups:
            if (v) out.fx.groups = v;
            break;
        case config::FixFn::Wings:
            if (v) out.fx.wings = v;
            break;
        default: break;
        }
        at += config::profile_slot_width(s);
    }
}

// Draws one fixture — `n` pixels at `d` — as its frame asks. `get_effect(index,
// effect&)` copies an effect of the bank, false when there is none there.
// With no effect (bank channel at 0, absent, or pointing nowhere) the fixture
// shows colour 1, steady, plus its white LED; with one, a colour of the desk
// replaces the effect's unless its three channels are at 0. The phaser and
// Block / Groups / Wings channels act either way.
// `over_pixels`: the output is pixel-mapped too and `d` holds the fixture's
// pixels. With the bank channel at 0 they stay, under the fixture's dimmer
// and shutter (its colour channels mean nothing next to pixel data); at 1-7
// (kBankColour) or on an effect, what the desk asks replaces them. `clocks`: the fixture's two
// (clock_tick: the look it shows and the one it fades from), nullptr for the wall clock.
template <typename GetEffect>
inline void render_fixture(uint8_t* d, size_t cap, uint16_t n, uint8_t bpp, const FixtureFrame& f,
                           bool reversed, uint64_t phase_ms, GetEffect get_effect,
                           bool over_pixels = false, FxClock* clocks = nullptr, uint32_t seed = 0) {
    const size_t total = static_cast<size_t>(n) * bpp;
    if (total > cap || bpp == 0 || n == 0) return;
    // The dimmer under the shutter and the strobe: a closed or dark fixture
    // draws nothing at all.
    const uint32_t lit = static_cast<uint32_t>(shutter_level(f.shutter, phase_ms, seed)) *
                         shutter_level(f.strobe, phase_ms, seed) / 255u;
    const auto level = static_cast<uint16_t>(static_cast<uint32_t>(f.dimmer) * lit / 255u);
    if (level == 0) {
        std::memset(d, 0, total);
        return;
    }
    config::Effect e{};
    const bool plain = f.bank < 0 || !get_effect(static_cast<size_t>(f.bank), e);
    if (plain && over_pixels && f.bank != kBankColour) {
        apply_master(d, total, level);
        return;
    }
    if (plain) {
        e            = config::Effect{};
        e.num_colors = 1;
    }
    for (size_t k = 0; k < config::kSceneColorsMax; ++k) {
        const int16_t* c = f.color[k];
        if (c[0] < 0 && c[1] < 0 && c[2] < 0) continue;  // not in the profile
        // On an effect, a colour at 0,0,0 is the effect's own; plain, colour
        // 1 is the fixture's colour whatever it is.
        if (c[0] <= 0 && c[1] <= 0 && c[2] <= 0 && !(plain && k == 0)) continue;
        if (plain && k > 0) continue;
        for (size_t j = 0; j < 3; ++j)
            e.colors[k][j] = static_cast<uint8_t>(c[j] < 0 ? 0 : c[j]);
        if (k + 1 > config::effect_num_colors(e)) e.num_colors = static_cast<uint8_t>(k + 1);
    }
    apply_effect_override(e, f.fx);
    // The fixture's own clock, keyed by the effect it plays: its Speed channel
    // bends the motion instead of jumping it.
    const auto key = static_cast<uint32_t>(f.bank + 1);
    fill_effect_run(d, cap, n, bpp, e,
                    clocks ? clock_tick(clock_slot(clocks, 2, key), key, e, phase_ms)
                           : wall_time(e, phase_ms));
    if (plain && bpp == 4 && f.white)
        for (uint16_t i = 0; i < n; ++i)
            d[static_cast<size_t>(i) * 4 + 3] = f.white;
    if (reversed) {
        const Span whole{ 0, n, true, 0 };
        reverse_fixtures(d, bpp, &whole, 1);
    }
    apply_master(d, total, level);
}

// What a fixture under fixture control keeps from frame to frame: the clocks
// of its looks and its effect transition (its FX fade channel).
struct FixtureFx {
    FxClock clock[2];  // the look it shows, the one it fades from
    LookFade fade;     // keyed by its Effect channel's pick
};

// Renders the fixtures of a channel under fixture control, each from its
// channels on the wire. `get_universe(number)` returns a 512-byte buffer or
// nullptr. Alone (`over_pixels` false) the strip starts dark: the fixtures of
// a missing universe and the pixels in no fixture stay so. Over a pixel-mapped
// output (`over_pixels`: `dst` holds the decoded pixels) they are left as
// they are, and each fixture keeps its pixels until its channels ask for an
// effect (render_fixture). `fx`: config::kMaxFixtures of them, one a fixture
// in patch order — nullptr draws on the wall clock and cuts on every change.
// `scratch` (dst_capacity bytes) is where a fading fixture draws the look it
// leaves; without it a change cuts.
template <typename GetUniverse, typename GetEffect>
inline void render_fixtures(uint8_t* dst, size_t dst_capacity, const config::ChannelConfig& cc,
                            const config::ProfileBank& bank, uint64_t phase_ms,
                            GetUniverse get_universe, GetEffect get_effect,
                            bool over_pixels = false, FixtureFx* fx = nullptr,
                            uint8_t* scratch = nullptr) {
    const uint8_t bpp  = led::bytes_per_pixel(cc.protocol);
    const size_t total = static_cast<size_t>(cc.pixel_count) * bpp;
    if (total > dst_capacity || total == 0) return;
    if (!over_pixels) std::memset(dst, 0, total);
    const auto now = static_cast<uint32_t>(phase_ms);
    size_t k       = 0;  // the fixture's rank: its state
    for_each_fixture_patch(cc, bank, [&](const FixturePatch& p) {
        FixtureFx* st = fx ? &fx[k] : nullptr;
        ++k;
        const uint8_t* src = get_universe(
            static_cast<uint16_t>(config::fix_universe(cc) + p.uni_off));
        const size_t at = static_cast<size_t>(p.first) * bpp;
        if (!src || at >= total) return;
        const size_t room = (total - at) / bpp;
        const auto n      = static_cast<uint16_t>(p.count < room ? p.count : room);
        uint8_t* d        = dst + at;
        const size_t len  = static_cast<size_t>(n) * bpp;
        FixtureFrame frame;
        decode_fixture(bank.profiles[p.profile], src + p.slot, frame);
        // The look it shows: its Effect pick once settled, faded from the last.
        bool fading = false;
        if (st) {
            frame.bank = static_cast<int16_t>(
                look_tick(st->fade, frame.bank, scratch ? frame.fx_fade_ms : 0, now));
            fading = scratch && look_fading(st->fade, now);
        }
        if (fading) std::memcpy(scratch, d, len);  // the pixels under it, for the old look too
        FxClock* clocks = st ? st->clock : nullptr;
        // Its address on the wire tells it apart: its own random strobe.
        const uint32_t seed = hash32(
            (static_cast<uint32_t>(config::fix_universe(cc) + p.uni_off) << 9) | p.slot);
        render_fixture(d, total - at, n, bpp, frame, p.reversed, phase_ms, get_effect, over_pixels,
                       clocks, seed);
        if (!fading) return;
        FixtureFrame old = frame;
        old.bank         = static_cast<int16_t>(st->fade.from);
        render_fixture(scratch, dst_capacity, n, bpp, old, p.reversed, phase_ms, get_effect,
                       over_pixels, clocks, seed);
        blend_into(d, scratch, len, fade_weight(now - st->fade.start, st->fade.len));
    });
}

}  // namespace pixfrog::dmx::logic
