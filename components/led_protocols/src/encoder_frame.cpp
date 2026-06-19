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
    if (d.pixel_count > 0) detail::transformed_pixel_bytes(d, pixels, 0, c.bytes);
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

    // Segment model. Split the bit cell at every distinct timing edge (each
    // channel's t0h and t1h, plus the cell end). Inside a run starting at sample
    // offset `start`, a channel is HIGH iff its high window exceeds `start` —
    // t0h for a 0-bit, t1h for a 1-bit. So with two per-run masks precomputed at
    // rebuild — A = channels whose t0h > start, B = channels whose t1h > start —
    // the bus word for that run is just (A & ~ones) | (B & ones): two bitwise
    // ops, no per-edge incremental masking, no sorted-edge struct scan per bit.
    struct Seg {
        uint16_t len;
        uint16_t a;
        uint16_t b;
    };
    Seg segs[2 * kMaxChannels + 1];
    size_t nsegs = 0;

    // Segments and the active set only change when a channel runs out of bits,
    // so they're rebuilt at byte 0 and at each dropout — not per bit. total_bits
    // is always a whole number of bytes (pixel_count × bpp × 8), so dropouts are
    // byte-aligned and all 8 bits of a byte share one segment table.
    const uint32_t max_bytes   = max_bits >> 3;
    uint32_t next_rebuild_byte = 0;
    uint32_t out_pos           = 0;

    size_t active[kMaxChannels];   // indices of channels still emitting
    uint16_t amask[kMaxChannels];  // their bus masks (cached for the inner loop)
    uint8_t cur[kMaxChannels];     // their current transformed byte
    size_t active_n = 0;

    for (uint32_t byte_pos = 0; byte_pos < max_bytes; ++byte_pos) {
        if (byte_pos == next_rebuild_byte) {
            active_n          = 0;
            next_rebuild_byte = max_bytes;
            // Sorted set of distinct edge positions (t0h/t1h of every active
            // channel + the cell end). Insertion sort — at most 2n+1 entries.
            uint16_t pos[2 * kMaxChannels + 1];
            size_t np          = 0;
            const auto add_pos = [&](uint16_t p) {
                size_t k = 0;
                while (k < np && pos[k] < p)
                    ++k;
                if (k == np || pos[k] != p) {
                    for (size_t m = np; m > k; --m)
                        pos[m] = pos[m - 1];
                    pos[k] = p;
                    ++np;
                }
            };
            for (size_t i = 0; i < n; ++i) {
                const uint32_t end_byte = ch[i].total_bits >> 3;
                if (end_byte <= byte_pos) continue;
                active[active_n] = i;
                amask[active_n]  = ch[i].mask;
                ++active_n;
                add_pos(ch[i].t0h);
                add_pos(ch[i].t1h);
                if (end_byte < next_rebuild_byte) next_rebuild_byte = end_byte;
            }
            add_pos(samples_bit);  // close the final run at the cell end

            // Each sorted position is a run end; the run starts at the previous
            // end (0 for the first). Precompute A/B from that start.
            uint16_t start = 0;
            nsegs          = 0;
            for (size_t k = 0; k < np; ++k) {
                uint16_t a = 0, b = 0;
                for (size_t ai = 0; ai < active_n; ++ai) {
                    const NrzChan& c = ch[active[ai]];
                    if (c.t0h > start) a = static_cast<uint16_t>(a | amask[ai]);
                    if (c.t1h > start) b = static_cast<uint16_t>(b | amask[ai]);
                }
                segs[nsegs].len = static_cast<uint16_t>(pos[k] - start);
                segs[nsegs].a   = a;
                segs[nsegs].b   = b;
                ++nsegs;
                start = pos[k];
            }
        }

        // Gather each active channel's byte once (8× fewer cursor advances and
        // pixel transforms than the per-bit path).
        for (size_t a = 0; a < active_n; ++a)
            cur[a] = chan_next_byte(ch[active[a]]);

        for (int bit = 7; bit >= 0; --bit) {
            uint16_t ones = 0;
            for (size_t a = 0; a < active_n; ++a) {
                if ((cur[a] >> bit) & 1u) ones = static_cast<uint16_t>(ones | amask[a]);
            }

            uint16_t* p = out + out_pos;
            for (size_t s = 0; s < nsegs; ++s) {
                const uint16_t word = static_cast<uint16_t>((segs[s].a & ~ones) |
                                                            (segs[s].b & ones));
                if (kStore) {
                    fill_run(p, word, segs[s].len);
                } else {
                    or_run(p, word, segs[s].len);
                }
                p += segs[s].len;
            }
            out_pos += samples_bit;
        }
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

// One clocked (APA102/SK9822/LPD8806) channel in the merged sweep. Like the NRZ
// case, separate per-channel encode_spi passes are read-modify-write traversals;
// merging every clocked channel sharing samples_per_clock into ONE sweep cuts
// that to a single OR pass over the group's span. The byte stream is identical
// to encode_spi (native protocol order, no color_order remap, brightness/LUT).
struct ClockedChan {
    const ChannelDesc* d;
    const uint8_t* pixels;
    uint16_t data_mask;
    uint16_t clk_mask;
    bool is_apa;  // APA102/SK9822: 4-byte start + per-pixel brightness byte
    uint8_t bpp;  // output bytes per pixel (4 APA, 3 LPD)
    uint32_t start_bytes;
    uint32_t data_end_byte;  // start_bytes + bpp × pixel_count
    uint32_t total_bits;     // total_bytes × 8
    uint32_t byte_idx;       // cursor over the byte stream
    uint16_t cur_pi;         // pixel whose transformed bytes are cached in cur3
    uint8_t pix_byte;        // byte index within the current pixel (0..bpp-1)
    uint8_t cur3[3];         // transformed r,g,b of cur_pi
};

// Transformed (grouping, invert, LUT, brightness) r,g,b for output pixel
// `out_pi`. Matches encode_spi exactly — clocked protocols ignore color_order
// and use their native wire order at byte assembly.
void clocked_pixel_bytes(const ChannelDesc& d, const uint8_t* pixels, uint16_t out_pi,
                         uint8_t out3[3]) {
    const uint16_t px_max = d.pixel_count;
    const uint16_t src_pi = d.invert_direction ? static_cast<uint16_t>(px_max - 1 - out_pi)
                                               : out_pi;
    const uint16_t group  = d.grouping ? d.grouping : 1;
    const uint8_t* p      = pixels + (src_pi / group) * 3;
    const uint8_t lr      = d.lut ? d.lut->r[p[0]] : p[0];
    const uint8_t lg      = d.lut ? d.lut->g[p[1]] : p[1];
    const uint8_t lb      = d.lut ? d.lut->b[p[2]] : p[2];
    out3[0]               = detail::apply_brightness(lr, d.brightness);
    out3[1]               = detail::apply_brightness(lg, d.brightness);
    out3[2]               = detail::apply_brightness(lb, d.brightness);
}

void clocked_init(ClockedChan& c, const ChannelDesc& d, const uint8_t* pixels) {
    c.d                      = &d;
    c.pixels                 = pixels;
    c.data_mask              = static_cast<uint16_t>(1u << d.bus_bit_data);
    c.clk_mask               = static_cast<uint16_t>(1u << d.bus_bit_clock);
    c.is_apa                 = (d.protocol == Protocol::APA102 || d.protocol == Protocol::SK9822);
    c.bpp                    = c.is_apa ? 4 : 3;
    c.start_bytes            = c.is_apa ? 4 : 0;
    const uint32_t px        = d.pixel_count;
    const uint32_t end_bytes = c.is_apa ? ((px + 15) / 16 + 1) : ((px + 31) / 32);
    c.data_end_byte          = c.start_bytes + c.bpp * px;
    c.total_bits             = (c.data_end_byte + end_bytes) * 8u;
    c.byte_idx               = 0;
    c.cur_pi                 = 0;
    c.pix_byte               = 0;
    // Pixel 0 is cached ahead of the first data byte (the APA start frame, if
    // any, doesn't touch pixel data). Channels with pixel_count==0 never reach
    // the data phase, so cur3 staying unset is fine.
    if (px > 0) clocked_pixel_bytes(d, pixels, 0, c.cur3);
}

// Next byte of the channel's wire stream (MSB emitted first), advancing the
// cursor. Must not be called past total_bits/8.
uint8_t clocked_next_byte(ClockedChan& c) {
    uint8_t out;
    if (c.byte_idx < c.start_bytes) {
        out = 0x00;  // APA start frame
    } else if (c.byte_idx < c.data_end_byte) {
        // pix_byte / cur_pi advance incrementally — no per-byte division.
        const uint8_t r = c.cur3[0], g = c.cur3[1], b = c.cur3[2];
        if (c.is_apa) {  // [0xFF brightness, B, G, R]
            out = (c.pix_byte == 0) ? 0xFF : (c.pix_byte == 1) ? b : (c.pix_byte == 2) ? g : r;
        } else {  // LPD8806: [0x80|G>>1, 0x80|R>>1, 0x80|B>>1]
            out = (c.pix_byte == 0) ? static_cast<uint8_t>(0x80 | (g >> 1))
                : (c.pix_byte == 1) ? static_cast<uint8_t>(0x80 | (r >> 1))
                                    : static_cast<uint8_t>(0x80 | (b >> 1));
        }
        if (++c.pix_byte == c.bpp) {
            c.pix_byte = 0;
            if (++c.cur_pi < c.d->pixel_count) {
                clocked_pixel_bytes(*c.d, c.pixels, c.cur_pi, c.cur3);
            }
        }
    } else {
        out = c.is_apa ? 0xFF : 0x00;  // APA end frame / LPD latch
    }
    ++c.byte_idx;
    return out;
}

// Merge clocked channels sharing samples_per_clock. Each bit spans spc samples:
// DATA held for the whole bit, CLOCK low for the first half then high. The clock
// is asserted for every active channel regardless of data, so clk_all is fixed
// per active set; only the data word varies per bit.
template <bool kStore>
uint32_t sweep_clocked(ClockedChan* ch, size_t n, uint16_t spc, uint16_t* out) {
    uint32_t max_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        if (ch[i].total_bits > max_bits) max_bits = ch[i].total_bits;
    }
    const uint32_t max_bytes = max_bits >> 3;
    const uint16_t half      = static_cast<uint16_t>(spc / 2);

    uint32_t next_rebuild_byte = 0;
    uint32_t out_pos           = 0;
    uint16_t clk_all           = 0;  // OR of active channels' clock masks

    size_t active[kMaxChannels];
    uint16_t dmask[kMaxChannels];
    uint8_t cur[kMaxChannels];
    size_t active_n = 0;

    for (uint32_t byte_pos = 0; byte_pos < max_bytes; ++byte_pos) {
        if (byte_pos == next_rebuild_byte) {
            active_n          = 0;
            clk_all           = 0;
            next_rebuild_byte = max_bytes;
            for (size_t i = 0; i < n; ++i) {
                const uint32_t end_byte = ch[i].total_bits >> 3;
                if (end_byte <= byte_pos) continue;
                active[active_n] = i;
                dmask[active_n]  = ch[i].data_mask;
                ++active_n;
                clk_all = static_cast<uint16_t>(clk_all | ch[i].clk_mask);
                if (end_byte < next_rebuild_byte) next_rebuild_byte = end_byte;
            }
        }

        for (size_t a = 0; a < active_n; ++a)
            cur[a] = clocked_next_byte(ch[active[a]]);

        for (int bit = 7; bit >= 0; --bit) {
            uint16_t data = 0;
            for (size_t a = 0; a < active_n; ++a) {
                if ((cur[a] >> bit) & 1u) data = static_cast<uint16_t>(data | dmask[a]);
            }
            uint16_t* p = out + out_pos;
            if (kStore) {
                fill_run(p, data, half);
                fill_run(p + half, static_cast<uint16_t>(data | clk_all),
                         static_cast<uint16_t>(spc - half));
            } else {
                or_run(p, data, half);
                or_run(p + half, static_cast<uint16_t>(data | clk_all),
                       static_cast<uint16_t>(spc - half));
            }
            out_pos += spc;
        }
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

    // Clocked SPI channels grouped by samples_per_clock (≤ 3 distinct rates in
    // practice), each group merged into one OR pass instead of per-channel ones.
    struct ClockedGroup {
        uint16_t spc;
        size_t n;
        ClockedChan chans[kMaxChannels];
    };
    ClockedGroup cgroups[3]{};
    size_t ncgroups = 0;
    size_t leftover_clk[kMaxChannels];
    size_t nleftover_clk = 0;
    for (size_t i = 0; i < channel_count; ++i) {
        if (!is_clocked(descs[i].protocol) || !pixels[i] || descs[i].pixel_count == 0) continue;
        const uint16_t spc = timing_for(descs[i].protocol, descs[i].clock_hz).samples_per_clock;
        ClockedGroup* g    = nullptr;
        for (size_t k = 0; k < ncgroups; ++k) {
            if (cgroups[k].spc == spc) {
                g = &cgroups[k];
                break;
            }
        }
        if (!g && ncgroups < sizeof(cgroups) / sizeof(cgroups[0])) {
            g      = &cgroups[ncgroups++];
            g->spc = spc;
            g->n   = 0;
        }
        if (!g || g->n == kMaxChannels) {
            if (nleftover_clk < kMaxChannels) leftover_clk[nleftover_clk++] = i;
            continue;
        }
        clocked_init(g->chans[g->n++], descs[i], pixels[i]);
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
    for (size_t k = 0; k < ncgroups; ++k) {
        sweep_clocked<false>(cgroups[k].chans, cgroups[k].n, cgroups[k].spc, out_samples);
    }
    for (size_t k = 0; k < nleftover_clk; ++k) {
        const size_t i = leftover_clk[k];
        detail::encode_spi(descs[i], pixels[i], out_samples, out_samples_capacity);
    }

    return frame_len;
}

}  // namespace pixfrog::led
