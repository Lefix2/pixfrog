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

// A sample offset within the bit cell where some channels may go low.
// clear0: channels whose T0H ends here (cleared when their bit is 0).
// clear1: channels whose T1H ends here (cleared when their bit is 1).
struct Edge {
    uint16_t pos;
    uint16_t clear0;
    uint16_t clear1;
};

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
                for (uint16_t k = pos; k < b; ++k)
                    out[out_pos + k] = word;
            } else if (word) {
                for (uint16_t k = pos; k < b; ++k) {
                    out[out_pos + k] = static_cast<uint16_t>(out[out_pos + k] | word);
                }
            }
            word = static_cast<uint16_t>(word &
                                         ~((edges[e].clear0 & ~ones) | (edges[e].clear1 & ones)));
            pos  = b;
        }
        out_pos += samples_bit;
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

    // Pass 1: the first group fully initializes [0, store_end) with pure
    // stores; everything beyond up to frame_len is zeroed so the OR passes
    // below land on defined data.
    size_t store_end = 0;
    if (ngroups > 0) {
        store_end = sweep_group<true>(groups[0].chans, groups[0].n, groups[0].samples_bit,
                                      out_samples);
    }
    if (store_end < frame_len) {
        std::memset(out_samples + store_end, 0, (frame_len - store_end) * sizeof(uint16_t));
    }

    // Pass 2: remaining NRZ groups (different samples_bit) OR on top.
    for (size_t k = 1; k < ngroups; ++k) {
        sweep_group<false>(groups[k].chans, groups[k].n, groups[k].samples_bit, out_samples);
    }

    // Pass 3: compact OR-encoders — leftover NRZ, clocked SPI, DMX512.
    for (size_t k = 0; k < nleftover; ++k) {
        const size_t i = leftover_nrz[k];
        detail::encode_nrz(descs[i], pixels[i], out_samples, out_samples_capacity);
    }
    for (size_t i = 0; i < channel_count; ++i) {
        const ChannelDesc& d = descs[i];
        if (!pixels[i]) continue;
        if (is_clocked(d.protocol)) {
            detail::encode_spi(d, pixels[i], out_samples, out_samples_capacity);
        } else if (is_dmx(d.protocol)) {
            detail::encode_dmx(d, pixels[i], out_samples, out_samples_capacity);
        }
    }

    return frame_len;
}

}  // namespace pixfrog::led
