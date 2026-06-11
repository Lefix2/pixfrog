// Host-side unit tests for dmx_manager::logic.

#include <cstdio>
#include <cstring>

#include "dmx_logic.h"

using namespace pixfrog;
using namespace pixfrog::dmx::logic;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long va = static_cast<long long>(a);                                                  \
        long long vb = static_cast<long long>(b);                                                  \
        if (va == vb) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__, __LINE__, #a,  \
                         #b, va, vb);                                                              \
        }                                                                                          \
    } while (0)

#define EXPECT_TRUE(a)                                                                             \
    do {                                                                                           \
        if (a) {                                                                                   \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #a);                      \
        }                                                                                          \
    } while (0)

// ── Sizing helpers ──────────────────────────────────────────────────────────

static void test_total_bytes_rgb() {
    config::ChannelConfig cc{};
    cc.protocol    = led::Protocol::WS2815;
    cc.pixel_count = 100;
    EXPECT_EQ(channel_total_bytes(cc), 300);
}

static void test_total_bytes_rgbw() {
    config::ChannelConfig cc{};
    cc.protocol    = led::Protocol::SK6812;
    cc.pixel_count = 100;
    EXPECT_EQ(channel_total_bytes(cc), 400);
}

static void test_universes_used() {
    config::ChannelConfig cc{};
    cc.protocol = led::Protocol::WS2815;

    cc.pixel_count = 100;
    EXPECT_EQ(channel_universes_used(cc), 1);  // 300 bytes → 1 universe

    cc.pixel_count = 200;
    EXPECT_EQ(channel_universes_used(cc), 2);  // 600 bytes → 2

    cc.pixel_count = 1024;
    EXPECT_EQ(channel_universes_used(cc), 6);  // 3072 bytes → 6
}

static void test_universes_off() {
    config::ChannelConfig cc{};
    cc.protocol    = led::Protocol::Off;
    cc.pixel_count = 1024;  // stale pixel_count is ignored when Off
    EXPECT_EQ(channel_total_bytes(cc), 0);
    EXPECT_EQ(channel_universes_used(cc), 0);  // disabled → claims no universe
}

// ── t_dma + capacity ────────────────────────────────────────────────────────

static void test_t_dma_ws2815() {
    config::ChannelConfig cc{};
    cc.protocol    = led::Protocol::WS2815;
    cc.pixel_count = 1024;
    // 1024 px × 24 bits × 20 samples + 4480 reset = 496 000 samples
    // / 16 MHz = 31 000 µs
    EXPECT_EQ(channel_t_dma_us(cc, led::kPclkHz), 31000);
}

static void test_emission_budget() {
    EXPECT_EQ(emission_budget_us(60), 1000000ULL / 60 - 1000);  // 15666
    EXPECT_EQ(emission_budget_us(30), 1000000ULL / 30 - 1000);  // 32333
    EXPECT_EQ(emission_budget_us(0), 0);
}

static void test_capacity_check() {
    config::ChannelConfig cc{};
    cc.protocol = led::Protocol::WS2815;

    // 555 px @ 60 Hz: t_dma ≈ 16930 µs > 15666 → FAIL
    cc.pixel_count = 555;
    EXPECT_TRUE(!channel_fits_budget(cc, led::kPclkHz, emission_budget_us(60)));

    // 300 px @ 60 Hz: t_dma ≈ 9280 µs ≤ 15666 → OK
    cc.pixel_count = 300;
    EXPECT_TRUE(channel_fits_budget(cc, led::kPclkHz, emission_budget_us(60)));

    // 1024 px @ 30 Hz: t_dma = 31000 µs ≤ 32333 → OK
    cc.pixel_count = 1024;
    EXPECT_TRUE(channel_fits_budget(cc, led::kPclkHz, emission_budget_us(30)));
}

// ── Decoder: single universe ────────────────────────────────────────────────

static void test_decode_single_universe() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 5;
    cc.universe_start = 1;
    cc.dmx_start      = 1;

    uint8_t universe1[512];
    for (int i = 0; i < 512; ++i)
        universe1[i] = static_cast<uint8_t>(i);

    auto get_universe = [&](uint16_t u) -> const uint8_t* {
        return (u == 1) ? universe1 : nullptr;
    };

    uint8_t pixels[32] = {};
    const bool ok      = decode_pixels(pixels, sizeof(pixels), cc, get_universe);
    EXPECT_TRUE(ok);
    // 5 px × 3 bytes = 15 bytes copied from offset 0.
    for (int i = 0; i < 15; ++i)
        EXPECT_EQ(pixels[i], static_cast<uint8_t>(i));
}

// ── Decoder: dmx_start offset ───────────────────────────────────────────────

static void test_decode_dmx_start_offset() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 2;
    cc.universe_start = 1;
    cc.dmx_start      = 10;  // 1-based → offset 9

    uint8_t universe1[512];
    for (int i = 0; i < 512; ++i)
        universe1[i] = static_cast<uint8_t>(i);

    auto get_universe = [&](uint16_t u) -> const uint8_t* {
        return (u == 1) ? universe1 : nullptr;
    };

    uint8_t pixels[16] = {};
    EXPECT_TRUE(decode_pixels(pixels, sizeof(pixels), cc, get_universe));
    // 6 bytes copied from offset 9.
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(pixels[i], static_cast<uint8_t>(9 + i));
}

// ── Decoder: multi-universe spanning ────────────────────────────────────────

static void test_decode_multi_universe() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 200;  // 600 bytes → spans 2 universes
    cc.universe_start = 5;
    cc.dmx_start      = 1;

    uint8_t universe5[512], universe6[512];
    for (int i = 0; i < 512; ++i) {
        universe5[i] = static_cast<uint8_t>(i);
        universe6[i] = static_cast<uint8_t>(0x80 | (i & 0x7F));
    }
    auto get_universe = [&](uint16_t u) -> const uint8_t* {
        if (u == 5) return universe5;
        if (u == 6) return universe6;
        return nullptr;
    };

    uint8_t pixels[800] = {};
    EXPECT_TRUE(decode_pixels(pixels, sizeof(pixels), cc, get_universe));
    EXPECT_EQ(pixels[0], 0);
    EXPECT_EQ(pixels[511], static_cast<uint8_t>(255));
    EXPECT_EQ(pixels[512], 0x80);
    EXPECT_EQ(pixels[599], static_cast<uint8_t>(0x80 | 87));
}

// ── Decoder: missing universe → zero-fill remainder, return false ───────────

static void test_decode_missing_universe() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 200;
    cc.universe_start = 5;
    cc.dmx_start      = 1;

    auto get_universe = [&](uint16_t /*u*/) -> const uint8_t* { return nullptr; };

    uint8_t pixels[700];
    std::memset(pixels, 0xFF, sizeof(pixels));
    EXPECT_TRUE(!decode_pixels(pixels, sizeof(pixels), cc, get_universe));
    for (int i = 0; i < 600; ++i)
        EXPECT_EQ(pixels[i], 0);
}

// ── Decoder: dst_capacity overflow → return false ───────────────────────────

static void test_decode_dst_too_small() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 100;  // 300 bytes
    cc.universe_start = 1;
    cc.dmx_start      = 1;

    uint8_t universe[512] = {};
    auto get_universe     = [&](uint16_t) -> const uint8_t* { return universe; };

    uint8_t pixels[16];  // too small
    EXPECT_TRUE(!decode_pixels(pixels, sizeof(pixels), cc, get_universe));
}

// ── Decoder: dmx_start past one universe → skips to next ───────────────────

static void test_decode_dmx_start_in_second_universe() {
    config::ChannelConfig cc{};
    cc.protocol       = led::Protocol::WS2815;
    cc.pixel_count    = 1;  // 3 bytes
    cc.universe_start = 1;
    cc.dmx_start      = 513;  // exactly past universe 1

    uint8_t universe1[512], universe2[512];
    std::memset(universe1, 0xAA, sizeof(universe1));
    std::memset(universe2, 0xBB, sizeof(universe2));
    auto get_universe = [&](uint16_t u) -> const uint8_t* {
        if (u == 1) return universe1;
        if (u == 2) return universe2;
        return nullptr;
    };

    uint8_t pixels[3] = {};
    EXPECT_TRUE(decode_pixels(pixels, sizeof(pixels), cc, get_universe));
    EXPECT_EQ(pixels[0], 0xBB);
    EXPECT_EQ(pixels[1], 0xBB);
    EXPECT_EQ(pixels[2], 0xBB);
}

// ── Pixel-count preview pattern ─────────────────────────────────────────────

static void test_preview_pattern_levels() {
    uint8_t buf[64 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 25, 3);

    // LED 1 (index 0): low
    EXPECT_EQ(buf[0], kPreviewLevelLow);
    EXPECT_EQ(buf[1], kPreviewLevelLow);
    EXPECT_EQ(buf[2], kPreviewLevelLow);
    // LED 10 and 20: decade marks at mid level
    EXPECT_EQ(buf[9 * 3], kPreviewLevelMid);
    EXPECT_EQ(buf[19 * 3], kPreviewLevelMid);
    // LED 25 (the count): full
    EXPECT_EQ(buf[24 * 3], kPreviewLevelFull);
    EXPECT_EQ(buf[24 * 3 + 2], kPreviewLevelFull);
    // LED 24: plain low
    EXPECT_EQ(buf[23 * 3], kPreviewLevelLow);
    // Nothing written past LED 25
    EXPECT_EQ(buf[25 * 3], 0);
}

static void test_preview_pattern_last_led_wins_over_decade() {
    // N = 30: LED 30 is both a decade mark and the count — full wins.
    uint8_t buf[32 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 30, 3);
    EXPECT_EQ(buf[29 * 3], kPreviewLevelFull);
    EXPECT_EQ(buf[19 * 3], kPreviewLevelMid);
}

static void test_preview_pattern_rgbw() {
    uint8_t buf[10 * 4] = {};
    fill_preview_pattern(buf, sizeof(buf), 10, 4);
    EXPECT_EQ(buf[0], kPreviewLevelLow);
    EXPECT_EQ(buf[3], kPreviewLevelLow);  // W byte of LED 1
    EXPECT_EQ(buf[9 * 4], kPreviewLevelFull);
}

static void test_preview_pattern_single_pixel() {
    uint8_t buf[3] = {};
    fill_preview_pattern(buf, sizeof(buf), 1, 3);
    EXPECT_EQ(buf[0], kPreviewLevelFull);
}

static void test_preview_pattern_overflow_is_noop() {
    uint8_t buf[3 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 4, 3);  // 12 bytes > 9-byte buffer
    EXPECT_EQ(buf[0], 0);
}

// ── Signal-loss failsafe ────────────────────────────────────────────────────

static void test_failsafe_due_logic() {
    // Disabled timeout → never due.
    EXPECT_TRUE(!failsafe_due(1'000'000, 100'000'000, 0));
    // Never active → never due, even with timeout set.
    EXPECT_TRUE(!failsafe_due(0, 100'000'000, 5));
    // Active 2 s ago, timeout 5 s → not due yet.
    EXPECT_TRUE(!failsafe_due(8'000'000, 10'000'000, 5));
    // Active 6 s ago, timeout 5 s → due.
    EXPECT_TRUE(failsafe_due(4'000'000, 10'500'000, 5));
    // Exactly at the boundary → not due (strictly greater).
    EXPECT_TRUE(!failsafe_due(5'000'000, 10'000'000, 5));
}

static void test_failsafe_fill_blackout() {
    uint8_t buf[4 * 3];
    std::memset(buf, 0xAA, sizeof(buf));
    fill_failsafe_pattern(buf, sizeof(buf), 4, 3, 1 /*blackout*/, 10, 20, 30);
    for (size_t i = 0; i < sizeof(buf); ++i)
        EXPECT_EQ(buf[i], 0);
}

static void test_failsafe_fill_color_rgb() {
    uint8_t buf[3 * 3] = {};
    fill_failsafe_pattern(buf, sizeof(buf), 3, 3, 2 /*colour*/, 0x40, 0x20, 0x10);
    EXPECT_EQ(buf[0], 0x40);
    EXPECT_EQ(buf[1], 0x20);
    EXPECT_EQ(buf[2], 0x10);
    EXPECT_EQ(buf[6], 0x40);  // pixel 3
    EXPECT_EQ(buf[8], 0x10);
}

static void test_failsafe_fill_color_rgbw_white_off() {
    uint8_t buf[2 * 4];
    std::memset(buf, 0xFF, sizeof(buf));
    fill_failsafe_pattern(buf, sizeof(buf), 2, 4, 2 /*colour*/, 1, 2, 3);
    EXPECT_EQ(buf[3], 0);  // W byte cleared
    EXPECT_EQ(buf[7], 0);
    EXPECT_EQ(buf[4], 1);
}

static void test_failsafe_fill_overflow_is_noop() {
    uint8_t buf[5] = {};
    fill_failsafe_pattern(buf, sizeof(buf), 2, 3, 2, 9, 9, 9);  // 6 bytes > 5
    EXPECT_EQ(buf[0], 0);
}

int main() {
    test_total_bytes_rgb();
    test_total_bytes_rgbw();
    test_universes_used();
    test_universes_off();
    test_t_dma_ws2815();
    test_emission_budget();
    test_capacity_check();
    test_decode_single_universe();
    test_decode_dmx_start_offset();
    test_decode_multi_universe();
    test_decode_missing_universe();
    test_decode_dst_too_small();
    test_decode_dmx_start_in_second_universe();
    test_preview_pattern_levels();
    test_preview_pattern_last_led_wins_over_decade();
    test_preview_pattern_rgbw();
    test_preview_pattern_single_pixel();
    test_preview_pattern_overflow_is_noop();
    test_failsafe_due_logic();
    test_failsafe_fill_blackout();
    test_failsafe_fill_color_rgb();
    test_failsafe_fill_color_rgbw_white_off();
    test_failsafe_fill_overflow_is_noop();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
