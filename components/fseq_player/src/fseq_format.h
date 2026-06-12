// Pure FSEQ v1/v2 header parser — no IDF, no RTOS, no allocation.
// All multi-byte fields are little-endian and match both x86 and Xtensa natively.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pixfrog::fseq {

// ── On-disk structures ───────────────────────────────────────────────────────

struct Header {
    uint8_t  magic[4];            // 'P','S','E','Q'
    uint16_t channel_data_offset; // file offset to first frame byte
    uint8_t  minor_version;
    uint8_t  major_version;       // 1 or 2
    uint16_t header_length;       // total byte length of all header sections
    uint32_t channel_count;       // bytes per frame
    uint32_t frame_count;
    uint8_t  step_time_ms;        // frame period (e.g. 25 → 40 Hz)
    uint8_t  flags;
    uint8_t  compression_type;    // 0=none 1=zstd 2=lz4
    uint8_t  num_comp_blocks;
    uint8_t  num_sparse_ranges;
    uint8_t  reserved;
    uint8_t  uuid[8];
} __attribute__((packed));

static_assert(sizeof(Header) == 32, "FSEQ header must be 32 bytes");

// Immediately after the fixed header (if compression_type != 0).
struct CompBlock {
    uint32_t first_frame; // first frame index in this block
    uint32_t data_size;   // compressed byte count
} __attribute__((packed));

static_assert(sizeof(CompBlock) == 8, "CompBlock must be 8 bytes");

// After the comp-block table (if num_sparse_ranges > 0).
struct SparseRange {
    uint32_t start_channel; // 0-based absolute channel index
    uint16_t length;        // channels in this range
} __attribute__((packed));

static_assert(sizeof(SparseRange) == 6, "SparseRange must be 6 bytes");

// ── Compression type constants ───────────────────────────────────────────────

constexpr uint8_t kCompNone = 0;
constexpr uint8_t kCompZstd = 1;
constexpr uint8_t kCompLz4  = 2;

// ── Parse result ─────────────────────────────────────────────────────────────

enum class ParseResult : uint8_t {
    Ok,
    TooShort,
    BadMagic,
    BadVersion,
    BadOffsets,
};

inline ParseResult parse_header(const uint8_t* buf, size_t len, Header& out) {
    if (len < sizeof(Header)) return ParseResult::TooShort;
    memcpy(&out, buf, sizeof(Header));
    if (out.magic[0] != 'P' || out.magic[1] != 'S' ||
        out.magic[2] != 'E' || out.magic[3] != 'Q')
        return ParseResult::BadMagic;
    if (out.major_version != 2)
        return ParseResult::BadVersion;
    if (out.channel_data_offset < static_cast<uint16_t>(sizeof(Header)))
        return ParseResult::BadOffsets;
    return ParseResult::Ok;
}

// ── Header helpers ───────────────────────────────────────────────────────────

// Byte offset in the file of the comp-block table.
constexpr size_t kCompBlockTableOffset = sizeof(Header); // 32

// Byte offset in the file of the sparse-range table.
inline size_t sparse_range_file_offset(const Header& h) {
    return kCompBlockTableOffset +
           static_cast<size_t>(h.num_comp_blocks) * sizeof(CompBlock);
}

// File byte offset of frame `n` for an uncompressed FSEQ.
inline uint32_t uncompressed_frame_offset(const Header& h, uint32_t n) {
    return static_cast<uint32_t>(h.channel_data_offset) + n * h.channel_count;
}

// ── Sparse-range helpers ─────────────────────────────────────────────────────

// Sum of all sparse-range lengths = actual bytes per frame when ranges are used.
inline uint32_t sparse_frame_bytes(const SparseRange* ranges, uint8_t count) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < count; ++i)
        total += ranges[i].length;
    return total;
}

// Map a per-frame byte offset `fseq_off` (within the sparse data) to the
// absolute 0-based channel number.  Returns UINT32_MAX if out of range.
inline uint32_t sparse_to_absolute(const SparseRange* ranges, uint8_t count,
                                    uint32_t fseq_off) {
    for (uint8_t i = 0; i < count; ++i) {
        if (fseq_off < static_cast<uint32_t>(ranges[i].length))
            return ranges[i].start_channel + fseq_off;
        fseq_off -= ranges[i].length;
    }
    return UINT32_MAX;
}

// Convert an absolute 0-based channel number to universe and slot.
// universe_base is typically 1 (Art-Net default first universe).
inline uint16_t channel_to_universe(uint32_t abs_ch, uint16_t universe_base) {
    return static_cast<uint16_t>(universe_base + abs_ch / 512u);
}
inline uint16_t channel_to_slot(uint32_t abs_ch) {
    return static_cast<uint16_t>(abs_ch % 512u);
}

// Number of universes needed to hold `channel_count` bytes.
inline size_t frame_universe_count(uint32_t channel_count) {
    return (channel_count + 511u) / 512u;
}

} // namespace pixfrog::fseq
