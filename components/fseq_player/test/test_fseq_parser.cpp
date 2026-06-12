// Host unit tests for fseq_format.h (pure parser, no IDF, no RTOS).

#include <cassert>
#include <cstdio>
#include <cstring>

#include "fseq_format.h"

using namespace pixfrog::fseq;

// ── Helpers ──────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);                        \
            ++g_fail;                                                                              \
        } else {                                                                                   \
            ++g_pass;                                                                              \
        }                                                                                          \
    } while (0)

static void build_header(uint8_t* buf, uint16_t data_offset, uint8_t major, uint32_t channel_count,
                         uint32_t frame_count, uint8_t step_ms, uint8_t comp_type,
                         uint8_t num_comp_blocks, uint8_t num_sparse) {
    memset(buf, 0, 32);
    buf[0] = 'P';
    buf[1] = 'S';
    buf[2] = 'E';
    buf[3] = 'Q';
    buf[4] = static_cast<uint8_t>(data_offset & 0xFF);
    buf[5] = static_cast<uint8_t>(data_offset >> 8);
    buf[6] = 0;  // minor version
    buf[7] = major;
    // header_length (8..9): same as data_offset for simplicity
    buf[8] = static_cast<uint8_t>(data_offset & 0xFF);
    buf[9] = static_cast<uint8_t>(data_offset >> 8);
    // channel_count (10..13)
    buf[10] = static_cast<uint8_t>(channel_count & 0xFF);
    buf[11] = static_cast<uint8_t>((channel_count >> 8) & 0xFF);
    buf[12] = static_cast<uint8_t>((channel_count >> 16) & 0xFF);
    buf[13] = static_cast<uint8_t>((channel_count >> 24) & 0xFF);
    // frame_count (14..17)
    buf[14] = static_cast<uint8_t>(frame_count & 0xFF);
    buf[15] = static_cast<uint8_t>((frame_count >> 8) & 0xFF);
    buf[16] = static_cast<uint8_t>((frame_count >> 16) & 0xFF);
    buf[17] = static_cast<uint8_t>((frame_count >> 24) & 0xFF);
    buf[18] = step_ms;
    buf[19] = 0;  // flags
    buf[20] = comp_type;
    buf[21] = num_comp_blocks;
    buf[22] = num_sparse;
    buf[23] = 0;  // reserved
    // uuid (24..31): zeros
}

// ── Test: parse_header ───────────────────────────────────────────────────────

static void test_parse_good() {
    uint8_t buf[32];
    build_header(buf, 32, 2, 1536, 100, 25, kCompNone, 0, 0);
    Header h;
    CHECK(parse_header(buf, 32, h) == ParseResult::Ok);
    CHECK(h.major_version == 2);
    CHECK(h.channel_count == 1536);
    CHECK(h.frame_count == 100);
    CHECK(h.step_time_ms == 25);
    CHECK(h.compression_type == kCompNone);
    CHECK(h.channel_data_offset == 32);
}

static void test_parse_v1_rejected() {
    uint8_t buf[32];
    build_header(buf, 32, 1, 512, 50, 40, kCompNone, 0, 0);
    Header h;
    // v1 is not supported; parser should reject it.
    CHECK(parse_header(buf, 32, h) == ParseResult::BadVersion);
}

static void test_parse_too_short() {
    uint8_t buf[31];
    memset(buf, 0, sizeof(buf));
    Header h;
    CHECK(parse_header(buf, 31, h) == ParseResult::TooShort);
}

static void test_parse_bad_magic() {
    uint8_t buf[32];
    build_header(buf, 32, 2, 512, 1, 25, kCompNone, 0, 0);
    buf[0] = 'X';
    Header h;
    CHECK(parse_header(buf, 32, h) == ParseResult::BadMagic);
}

static void test_parse_bad_version() {
    uint8_t buf[32];
    build_header(buf, 32, 3, 512, 1, 25, kCompNone, 0, 0);
    Header h;
    CHECK(parse_header(buf, 32, h) == ParseResult::BadVersion);
}

static void test_parse_bad_offset() {
    uint8_t buf[32];
    build_header(buf, 16, 2, 512, 1, 25, kCompNone, 0, 0);
    Header h;
    // data_offset=16 < sizeof(Header)=32
    CHECK(parse_header(buf, 32, h) == ParseResult::BadOffsets);
}

// ── Test: uncompressed frame offset ─────────────────────────────────────────

static void test_uncompressed_offset() {
    uint8_t buf[32];
    build_header(buf, 32, 2, 512, 10, 25, kCompNone, 0, 0);
    Header h;
    parse_header(buf, 32, h);

    CHECK(uncompressed_frame_offset(h, 0) == 32);
    CHECK(uncompressed_frame_offset(h, 1) == 32 + 512);
    CHECK(uncompressed_frame_offset(h, 9) == 32 + 9 * 512);
}

// ── Test: sparse range helpers ───────────────────────────────────────────────

static void test_sparse_frame_bytes() {
    SparseRange r[2];
    r[0].start_channel = 0;
    r[0].length        = 512;
    r[1].start_channel = 1024;
    r[1].length        = 256;
    CHECK(sparse_frame_bytes(r, 2) == 768);
    CHECK(sparse_frame_bytes(r, 0) == 0);
}

static void test_sparse_to_absolute() {
    SparseRange r[2];
    r[0].start_channel = 0;
    r[0].length        = 512;
    r[1].start_channel = 1024;
    r[1].length        = 256;

    // First range: fseq offset 0..511 → absolute 0..511
    CHECK(sparse_to_absolute(r, 2, 0) == 0);
    CHECK(sparse_to_absolute(r, 2, 511) == 511);
    // Second range: fseq offset 512..767 → absolute 1024..1279
    CHECK(sparse_to_absolute(r, 2, 512) == 1024);
    CHECK(sparse_to_absolute(r, 2, 767) == 1279);
    // Out of range
    CHECK(sparse_to_absolute(r, 2, 768) == UINT32_MAX);
}

// ── Test: channel → universe/slot conversion ─────────────────────────────────

static void test_channel_to_universe() {
    // universe_base = 1
    CHECK(channel_to_universe(0, 1) == 1);
    CHECK(channel_to_universe(511, 1) == 1);
    CHECK(channel_to_universe(512, 1) == 2);
    CHECK(channel_to_universe(1023, 1) == 2);
    CHECK(channel_to_universe(1024, 1) == 3);

    CHECK(channel_to_slot(0) == 0);
    CHECK(channel_to_slot(511) == 511);
    CHECK(channel_to_slot(512) == 0);
    CHECK(channel_to_slot(513) == 1);
}

// ── Test: frame_universe_count ───────────────────────────────────────────────

static void test_frame_universe_count() {
    CHECK(frame_universe_count(0) == 0);
    CHECK(frame_universe_count(1) == 1);
    CHECK(frame_universe_count(512) == 1);
    CHECK(frame_universe_count(513) == 2);
    CHECK(frame_universe_count(1536) == 3);
    CHECK(frame_universe_count(1537) == 4);
}

// ── Test: comp-block and sparse table offsets ────────────────────────────────

static void test_table_offsets() {
    uint8_t buf[32];
    build_header(buf, 48, 2, 512, 4, 25, kCompZstd, 2, 0);
    Header h;
    parse_header(buf, 32, h);

    // Comp-block table starts at offset 32.
    // sparse ranges start at 32 + 2*8 = 48.
    CHECK(sparse_range_file_offset(h) == 48);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    test_parse_good();
    test_parse_v1_rejected();
    test_parse_too_short();
    test_parse_bad_magic();
    test_parse_bad_version();
    test_parse_bad_offset();
    test_uncompressed_offset();
    test_sparse_frame_bytes();
    test_sparse_to_absolute();
    test_channel_to_universe();
    test_frame_universe_count();
    test_table_offsets();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
