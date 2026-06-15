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

inline void fill_preview_pattern(uint8_t* dst, size_t dst_capacity, uint16_t lit_count,
                                 uint16_t emit_count, uint8_t bytes_per_pixel) {
    if (emit_count < lit_count) emit_count = lit_count;
    const size_t total = static_cast<size_t>(emit_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0) return;
    for (uint16_t i = 1; i <= emit_count; ++i) {
        PreviewRGB c{ 0, 0, 0 };  // erase tail (i > lit_count) stays black
        if (i <= lit_count) {
            c = kPreviewGreen;
            if (i % 10 == 0) c = kPreviewYellow;
            if (i % 100 == 0) c = kPreviewPink;     // centade wins over decade
            if (i == lit_count) c = kPreviewWhite;  // the count wins over all
        }
        uint8_t* p = dst + static_cast<size_t>(i - 1) * bytes_per_pixel;
        p[0]       = c.r;
        if (bytes_per_pixel > 1) p[1] = c.g;
        if (bytes_per_pixel > 2) p[2] = c.b;
        for (uint8_t k = 3; k < bytes_per_pixel; ++k)
            p[k] = 0;  // W die stays dark
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

// Renders one frame of scene `effect` at time `phase_ms`:
//   solid   — r,g,b everywhere
//   chase   — black background, `param`-pixel head moving at `speed` px/s
//   rainbow — `param` wheel repeats along the strip, rotating with `speed`
inline void fill_scene_pattern(uint8_t* dst, size_t dst_capacity, uint16_t pixel_count,
                               uint8_t bytes_per_pixel, uint8_t effect, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t speed, uint8_t param, uint32_t phase_ms) {
    const size_t total = static_cast<size_t>(pixel_count) * bytes_per_pixel;
    if (total > dst_capacity || bytes_per_pixel == 0 || pixel_count == 0) return;

    switch (effect) {
    case 1: {  // chase
        std::memset(dst, 0, total);
        const uint16_t width = param ? param : 1;
        const uint32_t head  = (static_cast<uint64_t>(phase_ms) * speed / 1000) % pixel_count;
        for (uint16_t k = 0; k < width && k < pixel_count; ++k) {
            const uint16_t i = static_cast<uint16_t>((head + pixel_count - k) % pixel_count);
            set_px(dst, bytes_per_pixel, i, r, g, b);
        }
        break;
    }
    case 2: {  // rainbow
        const uint32_t repeats = param ? param : 1;
        const uint32_t offset  = (static_cast<uint64_t>(phase_ms) * speed / 100) % 360;
        for (uint16_t i = 0; i < pixel_count; ++i) {
            const uint32_t hue = (static_cast<uint32_t>(i) * 360 * repeats / pixel_count + offset);
            uint8_t pr, pg, pb;
            hue_to_rgb(hue, &pr, &pg, &pb);
            set_px(dst, bytes_per_pixel, i, pr, pg, pb);
        }
        break;
    }
    default:  // solid
        for (uint16_t i = 0; i < pixel_count; ++i)
            set_px(dst, bytes_per_pixel, i, r, g, b);
        break;
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

}  // namespace pixfrog::dmx::logic
