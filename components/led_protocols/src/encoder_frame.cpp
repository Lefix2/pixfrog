// Single-pass frame encoder.
//
// Per-channel encode_channel passes are read-modify-write traversals of the
// whole frame region: 8 NRZ channels cost 8 full PSRAM round-trips plus a
// memset, which alone busts the 60 FPS budget. This encoder merges every NRZ
// channel into ONE traversal of pure stores (the buffer needs no pre-zeroing),
// then ORs the compact clocked/DMX channels on top. PSRAM traffic per frame is
// one buffer write regardless of channel count.
//
// NRZ channels sharing the same samples_bit are swept bit-synchronously: for
// each bit time, the 16-bit bus word changes only at a handful of "edges"
// (the distinct T0H/T1H values plus the bit boundary), so each bit time is a
// few constant-word fills. Channels with a different samples_bit (SK6812 is
// 19 where WS281x are 20) form a second group, OR-ed over the stored one.
//
// DMX512 channels form their own sweep family: the whole waveform (BREAK 1536,
// MAB 192, 11-bit 8N2 characters at 64 samples/bit) is a sequence of 64-sample
// cells, each at a constant level, so all DMX channels merge cell-by-cell.
// A full universe spans 363 k samples (22.7 ms — DMX's own 44 Hz physical
// cap), the largest region a frame can have; without this it would be 8
// read-modify-write traversals again.
//
// The group with the largest extent gets the pure-store pass; the others are
// OR-ed over it, paying read-modify-write only on their own (smaller) span.

#include <cstring>

#include "encoder_dmx.h"
#include "encoder_nrz.h"
#include "encoder_spi.h"
#include "led_protocols.h"

namespace pixfrog::led {

namespace {

// 16-bit bus = 8 data+clock pairs, so at most 8 simultaneous channels.
constexpr size_t kMaxChannels = 8;

// One NRZ channel inside a sweep group: timing constants plus a lazy cursor
// over the transformed (wire-order, brightness, grouping, invert) bit stream.
struct NrzChan {
    const ChannelDesc* d;
    const uint8_t* pixels;
    uint16_t mask;
    uint16_t t0h;
    uint16_t t1h;
    uint32_t total_bits;
    size_t bytes_per_px;
    uint16_t out_pi;
    uint8_t byte_idx;
    uint8_t bit_idx;
    uint8_t bytes[4];
};

void chan_init(NrzChan& c, const ChannelDesc& d, const uint8_t* pixels, const Timing& t) {
    c.d            = &d;
    c.pixels       = pixels;
    c.mask         = static_cast<uint16_t>(1u << d.bus_bit_data);
    c.t0h          = t.samples_t0h;
    c.t1h          = t.samples_t1h;
    c.bytes_per_px = bytes_per_pixel(d.protocol);
    c.total_bits   = static_cast<uint32_t>(d.pixel_count) * c.bytes_per_px * 8u;
    c.out_pi       = 0;
    c.byte_idx     = 0;
    c.bit_idx      = 0;
    if (d.pixel_count > 0) detail::transformed_pixel_bytes(d, pixels, 0, c.bytes);
}

// Returns the current bit (MSB-first within each byte, bytes in wire order)
// and advances the cursor. Must not be called past total_bits.
bool chan_next_bit(NrzChan& c) {
    const bool bit = (c.bytes[c.byte_idx] >> (7 - c.bit_idx)) & 1u;
    if (++c.bit_idx == 8) {
        c.bit_idx = 0;
        if (++c.byte_idx == c.bytes_per_px) {
            c.byte_idx = 0;
            if (++c.out_pi < c.d->pixel_count) {
                detail::transformed_pixel_bytes(*c.d, c.pixels, c.out_pi, c.bytes);
            }
        }
    }
    return bit;
}

// Fill / OR a run of `n` identical samples into the 16-bit DMA buffer. Plain
// sequential 16-bit stores: the buffer is cache-backed PSRAM whose write-back
// already coalesces sequential writes into full cache-line evictions, so wider
// stores don't cut the (eviction-bound) memory traffic — they only add
// alignment-handling overhead, which measured *slower* on the P4.
inline void fill_run(uint16_t* d, uint16_t v, uint16_t n) {
    for (uint16_t k = 0; k < n; ++k)
        d[k] = v;
}

inline void or_run(uint16_t* d, uint16_t v, uint16_t n) {
    if (v == 0) return;
    for (uint16_t k = 0; k < n; ++k)
        d[k] = static_cast<uint16_t>(d[k] | v);
}

// Returns the current transformed byte (MSB is emitted first) and advances the
// cursor a whole byte. The homogeneous sweep gathers all 8 bits of a byte in
// one go, so the pixel-transform / cursor bookkeeping runs once per byte rather
// than once per bit (8× fewer cursor advances on the hot path).
uint8_t chan_next_byte(NrzChan& c) {
    const uint8_t b = c.bytes[c.byte_idx];
    if (++c.byte_idx == c.bytes_per_px) {
        c.byte_idx = 0;
        if (++c.out_pi < c.d->pixel_count) {
            detail::transformed_pixel_bytes(*c.d, c.pixels, c.out_pi, c.bytes);
        }
    }
    return b;
}

// A sample offset within the bit cell where some channels may go low.
// clear0: channels whose T0H ends here (cleared when their bit is 0).
// clear1: channels whose T1H ends here (cleared when their bit is 1).
struct Edge {
    uint16_t pos;
    uint16_t clear0;
    uint16_t clear1;
};

// Fast path for a group whose channels all share the same T0H and T1H (any
// single-protocol group — the common 8×WS2815 case). The bus word inside each
// bit is then exactly three constant runs: all active channels high until T0H,
// only the bit-1 channels high until T1H, all low after. This skips the generic
// edge table and its per-bit word masking entirely.
template <bool kStore>
uint32_t sweep_group_homogeneous(NrzChan* ch, size_t n, uint16_t samples_bit, uint16_t* out,
                                 uint32_t max_bits) {
    const uint16_t t0h  = ch[0].t0h;
    const uint16_t t1h  = ch[0].t1h;
    const uint16_t run1 = static_cast<uint16_t>(t1h - t0h);
    const uint16_t run2 = static_cast<uint16_t>(samples_bit - t1h);
    // total_bits is always a whole number of bytes (pixel_count × bpp × 8).
    const uint32_t max_bytes = max_bits >> 3;

    uint32_t next_rebuild_byte = 0;  // byte index at which some channel drops out
    uint16_t all_mask          = 0;
    size_t active_n            = 0;
    uint32_t out_pos           = 0;

    size_t active[kMaxChannels];   // indices of channels still emitting
    uint16_t amask[kMaxChannels];  // their bus masks (cached for the inner loop)
    uint8_t cur[kMaxChannels];     // their current transformed byte

    for (uint32_t byte_pos = 0; byte_pos < max_bytes; ++byte_pos) {
        // all_mask / the active set only change when a (shorter) channel ends.
        if (byte_pos == next_rebuild_byte) {
            all_mask          = 0;
            active_n          = 0;
            next_rebuild_byte = max_bytes;
            for (size_t i = 0; i < n; ++i) {
                const uint32_t end_byte = ch[i].total_bits >> 3;
                if (end_byte <= byte_pos) continue;
                active[active_n] = i;
                amask[active_n]  = ch[i].mask;
                ++active_n;
                all_mask = static_cast<uint16_t>(all_mask | ch[i].mask);
                if (end_byte < next_rebuild_byte) next_rebuild_byte = end_byte;
            }
        }

        for (size_t a = 0; a < active_n; ++a)
            cur[a] = chan_next_byte(ch[active[a]]);

        for (int bit = 7; bit >= 0; --bit) {
            uint16_t ones = 0;
            for (size_t a = 0; a < active_n; ++a) {
                if ((cur[a] >> bit) & 1u) ones = static_cast<uint16_t>(ones | amask[a]);
            }
            uint16_t* p = out + out_pos;
            if (kStore) {
                fill_run(p, all_mask, t0h);
                fill_run(p + t0h, ones, run1);
                fill_run(p + t1h, 0, run2);
            } else {
                or_run(p, all_mask, t0h);
                or_run(p + t0h, ones, run1);
                // [t1h, samples_bit) is low — nothing to OR in.
            }
            out_pos += samples_bit;
        }
    }
    return out_pos;
}

// Sweep one group of channels sharing `samples_bit`. kStore writes every
// sample (fully initializing the region); otherwise only non-zero words are
// OR-ed so other groups' bits survive. Returns samples written
// (= max_bits × samples_bit, excluding reset tails).
template <bool kStore>
uint32_t sweep_group(NrzChan* ch, size_t n, uint16_t samples_bit, uint16_t* out) {
    uint32_t max_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        if (ch[i].total_bits > max_bits) max_bits = ch[i].total_bits;
    }

    // Single-protocol group → the three-run fast path.
    bool homogeneous = true;
    for (size_t i = 1; i < n; ++i) {
        if (ch[i].t0h != ch[0].t0h || ch[i].t1h != ch[0].t1h) {
            homogeneous = false;
            break;
        }
    }
    if (homogeneous) return sweep_group_homogeneous<kStore>(ch, n, samples_bit, out, max_bits);

    Edge edges[2 * kMaxChannels + 1];
    size_t nedges     = 0;
    uint16_t all_mask = 0;

    const auto add_edge = [&](uint16_t pos, uint16_t mask, bool on_zero) {
        size_t k = 0;
        while (k < nedges && edges[k].pos < pos)
            ++k;
        if (k == nedges || edges[k].pos != pos) {
            for (size_t m = nedges; m > k; --m)
                edges[m] = edges[m - 1];
            edges[k] = Edge{ pos, 0, 0 };
            ++nedges;
        }
        if (on_zero) {
            edges[k].clear0 = static_cast<uint16_t>(edges[k].clear0 | mask);
        } else {
            edges[k].clear1 = static_cast<uint16_t>(edges[k].clear1 | mask);
        }
    };

    // The edge table only changes when a channel runs out of bits, so it is
    // rebuilt at bt=0 and at each channel dropout — not per bit.
    uint32_t next_rebuild = 0;
    uint32_t out_pos      = 0;

    for (uint32_t bt = 0; bt < max_bits; ++bt) {
        if (bt == next_rebuild) {
            nedges       = 0;
            all_mask     = 0;
            next_rebuild = max_bits;
            for (size_t i = 0; i < n; ++i) {
                if (ch[i].total_bits <= bt) continue;
                all_mask = static_cast<uint16_t>(all_mask | ch[i].mask);
                add_edge(ch[i].t0h, ch[i].mask, true);
                add_edge(ch[i].t1h, ch[i].mask, false);
                if (ch[i].total_bits < next_rebuild) next_rebuild = ch[i].total_bits;
            }
            add_edge(samples_bit, 0, true);  // terminator: fill to end of cell
        }

        uint16_t ones = 0;
        for (size_t i = 0; i < n; ++i) {
            if (ch[i].total_bits > bt && chan_next_bit(ch[i])) {
                ones = static_cast<uint16_t>(ones | ch[i].mask);
            }
        }

        uint16_t word = all_mask;  // every active channel starts its bit high
        uint16_t pos  = 0;
        for (size_t e = 0; e < nedges; ++e) {
            const uint16_t b = edges[e].pos;
            if (kStore) {
                fill_run(out + out_pos + pos, word, static_cast<uint16_t>(b - pos));
            } else {
                or_run(out + out_pos + pos, word, static_cast<uint16_t>(b - pos));
            }
            word = static_cast<uint16_t>(word &
                                         ~((edges[e].clear0 & ~ones) | (edges[e].clear1 & ones)));
            pos  = b;
        }
        out_pos += samples_bit;
    }
    return out_pos;
}

// One DMX512 channel in the cell sweep. The line is differential: data_mask
// when the level is high, comp_mask when low — exactly one of the two is
// asserted in every cell, matching encode_dmx.
struct DmxChan {
    uint16_t data_mask;
    uint16_t comp_mask;
    uint32_t total_cells;
    const uint8_t* slots;
    uint16_t slot_count;
    uint32_t pos;         // cell cursor
    uint32_t char_idx;    // 0 = null start code, 1..slot_count = data slots
    uint8_t bit_in_char;  // 0 start, 1..8 data LSB-first, 9..10 stop
    uint8_t cur_char;
};

// BREAK and MAB are exact multiples of the 64-sample bit cell.
constexpr uint32_t kDmxBreakCells = detail::kDmxBreakSamples / detail::kDmxSamplesPerBit;
constexpr uint32_t kDmxMabCells   = detail::kDmxMabSamples / detail::kDmxSamplesPerBit;

void dmx_init(DmxChan& c, const ChannelDesc& d, const uint8_t* slots) {
    c.data_mask   = static_cast<uint16_t>(1u << d.bus_bit_data);
    c.comp_mask   = static_cast<uint16_t>(1u << d.bus_bit_clock);
    c.slots       = slots;
    c.slot_count  = d.pixel_count;
    c.total_cells = kDmxBreakCells + kDmxMabCells +
                    (static_cast<uint32_t>(d.pixel_count) + 1) * detail::kDmxBitsPerSlot;
    c.pos         = 0;
    c.char_idx    = 0;
    c.bit_in_char = 0;
    c.cur_char    = 0x00;  // null start code
}

// Level mask for the current cell, then advance. Sequential calls only; must
// not be called past total_cells.
uint16_t dmx_next_mask(DmxChan& c) {
    bool high;
    if (c.pos < kDmxBreakCells) {
        high = false;
    } else if (c.pos < kDmxBreakCells + kDmxMabCells) {
        high = true;
    } else {
        if (c.bit_in_char == 0) {
            high = false;  // start bit
        } else if (c.bit_in_char <= 8) {
            high = ((c.cur_char >> (c.bit_in_char - 1)) & 1u) != 0;
        } else {
            high = true;  // stop bits
        }
        if (++c.bit_in_char == detail::kDmxBitsPerSlot) {
            c.bit_in_char = 0;
            ++c.char_idx;
            c.cur_char = (c.char_idx <= c.slot_count) ? c.slots[c.char_idx - 1] : 0;
        }
    }
    ++c.pos;
    return high ? c.data_mask : c.comp_mask;
}

template <bool kStore> uint32_t sweep_dmx(DmxChan* ch, size_t n, uint16_t* out) {
    uint32_t max_cells = 0;
    for (size_t i = 0; i < n; ++i) {
        if (ch[i].total_cells > max_cells) max_cells = ch[i].total_cells;
    }

    uint32_t out_pos = 0;
    for (uint32_t ct = 0; ct < max_cells; ++ct) {
        uint16_t word = 0;
        for (size_t i = 0; i < n; ++i) {
            if (ch[i].total_cells > ct) word = static_cast<uint16_t>(word | dmx_next_mask(ch[i]));
        }
        if (kStore) {
            fill_run(out + out_pos, word, detail::kDmxSamplesPerBit);
        } else {
            or_run(out + out_pos, word, detail::kDmxSamplesPerBit);
        }
        out_pos += detail::kDmxSamplesPerBit;
    }
    return out_pos;
}

}  // namespace

size_t encode_frame(const ChannelDesc* descs, const uint8_t* const* pixels, size_t channel_count,
                    uint16_t* out_samples, size_t out_samples_capacity) {
    size_t frame_len = 0;
    for (size_t i = 0; i < channel_count; ++i) {
        if (is_off(descs[i].protocol) || !pixels[i]) continue;
        const size_t len = encoded_size_samples(descs[i]);
        if (len > frame_len) frame_len = len;
    }
    if (frame_len == 0 || frame_len > out_samples_capacity) return 0;

    struct Group {
        uint16_t samples_bit;
        size_t n;
        NrzChan chans[kMaxChannels];
    };
    Group groups[3]{};
    size_t ngroups = 0;
    // Channels that don't fit the sweep (group/slot overflow) fall back to the
    // per-channel OR encoder over the initialized buffer.
    size_t leftover_nrz[kMaxChannels];
    size_t nleftover = 0;

    for (size_t i = 0; i < channel_count; ++i) {
        const ChannelDesc& d = descs[i];
        if (is_off(d.protocol) || is_dmx(d.protocol) || is_clocked(d.protocol)) continue;
        if (!pixels[i] || d.pixel_count == 0) continue;
        const Timing t = timing_for(d.protocol, d.clock_hz);
        Group* g       = nullptr;
        for (size_t k = 0; k < ngroups; ++k) {
            if (groups[k].samples_bit == t.samples_bit) {
                g = &groups[k];
                break;
            }
        }
        if (!g && ngroups < sizeof(groups) / sizeof(groups[0])) {
            g              = &groups[ngroups++];
            g->samples_bit = t.samples_bit;
            g->n           = 0;
        }
        if (!g || g->n == kMaxChannels) {
            if (nleftover < kMaxChannels) leftover_nrz[nleftover++] = i;
            continue;
        }
        chan_init(g->chans[g->n++], d, pixels[i], t);
    }

    DmxChan dmx_chans[kMaxChannels];
    size_t ndmx = 0;
    size_t leftover_dmx[kMaxChannels];
    size_t nleftover_dmx = 0;
    for (size_t i = 0; i < channel_count; ++i) {
        if (!is_dmx(descs[i].protocol) || !pixels[i]) continue;
        if (ndmx == kMaxChannels) {
            if (nleftover_dmx < kMaxChannels) leftover_dmx[nleftover_dmx++] = i;
            continue;
        }
        dmx_init(dmx_chans[ndmx++], descs[i], pixels[i]);
    }

    // Extents (samples each sweep would write, reset tails excluded — those
    // are zeros). The largest-extent sweep gets the pure-store pass; the
    // others pay read-modify-write only over their own smaller span.
    size_t extents[4]{};  // [0..2] NRZ groups, [3] DMX
    for (size_t k = 0; k < ngroups; ++k) {
        uint32_t max_bits = 0;
        for (size_t i = 0; i < groups[k].n; ++i) {
            if (groups[k].chans[i].total_bits > max_bits) max_bits = groups[k].chans[i].total_bits;
        }
        extents[k] = static_cast<size_t>(max_bits) * groups[k].samples_bit;
    }
    for (size_t i = 0; i < ndmx; ++i) {
        const size_t e = static_cast<size_t>(dmx_chans[i].total_cells) * detail::kDmxSamplesPerBit;
        if (e > extents[3]) extents[3] = e;
    }
    size_t store_idx = 0;
    for (size_t k = 1; k < 4; ++k) {
        if (extents[k] > extents[store_idx]) store_idx = k;
    }

    // Pass 1: the store sweep fully initializes [0, store_end); everything
    // beyond up to frame_len is zeroed so the OR passes land on defined data.
    size_t store_end = 0;
    if (store_idx == 3) {
        if (ndmx > 0) store_end = sweep_dmx<true>(dmx_chans, ndmx, out_samples);
    } else if (ngroups > 0) {
        store_end = sweep_group<true>(groups[store_idx].chans, groups[store_idx].n,
                                      groups[store_idx].samples_bit, out_samples);
    }
    if (store_end < frame_len) {
        std::memset(out_samples + store_end, 0, (frame_len - store_end) * sizeof(uint16_t));
    }

    // Pass 2: remaining sweeps OR on top.
    for (size_t k = 0; k < ngroups; ++k) {
        if (k == store_idx) continue;
        sweep_group<false>(groups[k].chans, groups[k].n, groups[k].samples_bit, out_samples);
    }
    if (store_idx != 3 && ndmx > 0) {
        sweep_dmx<false>(dmx_chans, ndmx, out_samples);
    }

    // Pass 3: compact OR-encoders — leftover NRZ and clocked SPI (small
    // regions, per-channel cost negligible).
    for (size_t k = 0; k < nleftover; ++k) {
        const size_t i = leftover_nrz[k];
        detail::encode_nrz(descs[i], pixels[i], out_samples, out_samples_capacity);
    }
    for (size_t k = 0; k < nleftover_dmx; ++k) {
        const size_t i = leftover_dmx[k];
        detail::encode_dmx(descs[i], pixels[i], out_samples, out_samples_capacity);
    }
    for (size_t i = 0; i < channel_count; ++i) {
        if (!pixels[i]) continue;
        if (is_clocked(descs[i].protocol)) {
            detail::encode_spi(descs[i], pixels[i], out_samples, out_samples_capacity);
        }
    }

    return frame_len;
}

}  // namespace pixfrog::led
