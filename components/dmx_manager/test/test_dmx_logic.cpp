// Host-side unit tests for dmx_manager::logic.

#include <cstdio>
#include <cstring>
#include <vector>

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

static void test_auto_patch_cascade() {
    config::ChannelConfig chans[8]{};
    chans[0].protocol    = led::Protocol::WS2815;
    chans[0].pixel_count = 100;  // 1 universe
    chans[1].protocol    = led::Protocol::WS2815;
    chans[1].pixel_count = 200;  // 2 universes
    chans[2].protocol    = led::Protocol::Off;
    chans[2].pixel_count = 144;  // disabled → 0 universes
    for (int i = 3; i < 8; ++i) {
        chans[i].protocol    = led::Protocol::WS2815;
        chans[i].pixel_count = 50;  // 1 universe each
    }

    uint16_t out[8]{};
    const uint16_t next = compute_auto_patch(14, chans, 8, out);
    EXPECT_EQ(out[0], 14);  // base
    EXPECT_EQ(out[1], 15);  // +1 (ch0 used 1)
    EXPECT_EQ(out[2], 17);  // +2 (ch1 used 2) — crosses into subnet 1 (15→16→17)
    EXPECT_EQ(out[3], 17);  // ch2 disabled used 0 → ch3 reuses the cursor
    EXPECT_EQ(out[4], 18);
    EXPECT_EQ(out[5], 19);
    EXPECT_EQ(out[6], 20);
    EXPECT_EQ(out[7], 21);
    EXPECT_EQ(next, 22);  // first free universe past the last channel
}

// ── universe → slot map ─────────────────────────────────────────────────────

// The table the firmware passes in is indexed by the 15-bit Port-Address.
// Allocating it on the heap (not the stack) keeps the 64 KiB off the test
// thread's stack.
struct UniMap {
    std::vector<uint16_t> uni_to_slot;
    std::vector<uint8_t> slot_to_chan;
    size_t unmapped = 0;
    uint16_t used   = 0;

    explicit UniMap(size_t num_slots)
        : uni_to_slot(kMaxUniverseNumber + 1), slot_to_chan(num_slots) {}

    void build(const config::ChannelConfig* chans, size_t n) {
        used = build_universe_map(chans, n, uni_to_slot.data(), slot_to_chan.data(),
                                  slot_to_chan.size(), &unmapped);
    }
};

static void fill_channels(config::ChannelConfig* chans, size_t n, led::Protocol proto,
                          uint16_t pixels, uint16_t first_universe, uint16_t stride) {
    for (size_t i = 0; i < n; ++i) {
        chans[i]                = config::ChannelConfig{};
        chans[i].protocol       = proto;
        chans[i].pixel_count    = pixels;
        chans[i].universe_start = static_cast<uint16_t>(first_universe + i * stride);
    }
}

static void test_universe_map_basic() {
    config::ChannelConfig chans[8];
    fill_channels(chans, 8, led::Protocol::WS2815, 50, 1, 1);  // 1 universe each

    UniMap m(64);
    m.build(chans, 8);
    EXPECT_EQ(m.used, 8);
    EXPECT_EQ(m.unmapped, 0);
    EXPECT_EQ(m.uni_to_slot[1], 0);
    EXPECT_EQ(m.uni_to_slot[8], 7);
    EXPECT_EQ(m.slot_to_chan[7], 7);
    // Everything else stays unmapped.
    EXPECT_EQ(m.uni_to_slot[0], kNoSlot);
    EXPECT_EQ(m.uni_to_slot[9], kNoSlot);
    EXPECT_EQ(m.uni_to_slot[kMaxUniverseNumber], kNoSlot);
}

// 1024 px RGBW = 4096 B = 8 universes per channel; 8 channels need all 64
// slots. This is the configuration that used to overflow a 48-slot pool.
static void test_universe_map_rgbw_fills_pool() {
    config::ChannelConfig chans[8];
    fill_channels(chans, 8, led::Protocol::SK6812, 1024, 1, 8);
    EXPECT_EQ(channel_universes_used(chans[0]), 8);

    UniMap m(64);
    m.build(chans, 8);
    EXPECT_EQ(m.used, 64);
    EXPECT_EQ(m.unmapped, 0);
    EXPECT_EQ(m.uni_to_slot[1], 0);
    EXPECT_EQ(m.uni_to_slot[64], 63);
    EXPECT_EQ(m.slot_to_chan[63], 7);
}

// A pool too small must report the shortfall rather than truncate silently,
// and must not write past slot_to_chan.
static void test_universe_map_pool_exhaustion_is_reported() {
    config::ChannelConfig chans[8];
    fill_channels(chans, 8, led::Protocol::SK6812, 1024, 1, 8);

    UniMap m(48);
    m.build(chans, 8);
    EXPECT_EQ(m.used, 48);
    EXPECT_EQ(m.unmapped, 16);  // 64 wanted, 48 available
}

// A channel patched at the top of the address space must not map (nor write)
// past kMaxUniverseNumber.
static void test_universe_map_clamps_at_top_of_range() {
    config::ChannelConfig chans[1];
    fill_channels(chans, 1, led::Protocol::SK6812, 1024, kMaxUniverseNumber, 0);
    EXPECT_EQ(channel_universes_used(chans[0]), 8);

    UniMap m(64);
    m.build(chans, 1);
    EXPECT_EQ(m.used, 1);      // only universe 32767 itself fits
    EXPECT_EQ(m.unmapped, 7);  // the other 7 fall off the end
    EXPECT_EQ(m.uni_to_slot[kMaxUniverseNumber], 0);
}

static void test_universe_routable_range() {
    EXPECT_EQ(universe_routable(0), 1);
    EXPECT_EQ(universe_routable(kMaxUniverseNumber), 1);
    EXPECT_EQ(universe_routable(kMaxUniverseNumber + 1), 0);
    EXPECT_EQ(universe_routable(63999), 0);  // top of the sACN range
    EXPECT_EQ(universe_routable(65535), 0);
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

// A DMX channel is judged at min(refresh, 44 Hz), not the LED refresh: a full
// 512-slot universe (~22.68 ms) is OK at any refresh, never flagged at 60 Hz.
static void test_capacity_dmx_ignores_led_refresh() {
    config::ChannelConfig cc{};
    cc.protocol = led::Protocol::DMX512;

    // 512 slots: 1536 + 192 + 513 × 704 = 362 880 samples / 16 MHz = 22 680 µs.
    cc.pixel_count = 512;
    EXPECT_EQ(channel_t_dma_us(cc, led::kPclkHz), 22680);
    // 22 680 ≤ 1e6/44 (22 727) → OK at 60 Hz (would FAIL the 15 666 LED budget).
    EXPECT_TRUE(channel_fits_refresh(cc, led::kPclkHz, 60));
    EXPECT_TRUE(channel_fits_refresh(cc, led::kPclkHz, 30));
    // The old LED-budget check is what used to (wrongly) reject it at 60 Hz.
    EXPECT_TRUE(!channel_fits_budget(cc, led::kPclkHz, emission_budget_us(60)));

    // An oversized DMX frame (600 slots ≈ 26 552 µs) genuinely can't hold 44 Hz.
    cc.pixel_count = 600;
    EXPECT_TRUE(!channel_fits_refresh(cc, led::kPclkHz, 60));

    // A normal LED channel still goes through the emission-budget path.
    cc.protocol    = led::Protocol::WS2815;
    cc.pixel_count = 555;  // ~16 930 µs > 15 666 → FAIL at 60 Hz
    EXPECT_TRUE(!channel_fits_refresh(cc, led::kPclkHz, 60));
    cc.pixel_count = 300;  // OK at 60 Hz
    EXPECT_TRUE(channel_fits_refresh(cc, led::kPclkHz, 60));
}

// ── max_pixels_for: inverse of the capacity check ───────────────────────────

static void test_max_pixels_ws2815() {
    config::ChannelConfig cc{};
    cc.protocol      = led::Protocol::WS2815;
    const size_t buf = led::kMaxSamplesPerFrame;

    // 60 Hz: budget 15 666 µs → 250 656 samples; (250656-4480)/480 = 512.86 → 512.
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 60, buf), 512);
    // 30 Hz: budget 32 333 µs fits well over 1024, so the absolute cap wins.
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 30, buf), 1024);

    // The result must pass the capacity check, and one pixel more must not.
    cc.pixel_count = max_pixels_for(cc, led::kPclkHz, 60, buf);
    EXPECT_TRUE(channel_fits_refresh(cc, led::kPclkHz, 60));
    cc.pixel_count = static_cast<uint16_t>(cc.pixel_count + 1);
    EXPECT_TRUE(!channel_fits_refresh(cc, led::kPclkHz, 60));
}

static void test_max_pixels_dmx() {
    config::ChannelConfig cc{};
    cc.protocol = led::Protocol::DMX512;
    // DMX is one universe regardless of refresh (judged at its own ≤44 Hz rate).
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 60, led::kMaxSamplesPerFrame), 512);
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 30, led::kMaxSamplesPerFrame), 512);
}

static void test_max_pixels_off_and_zero_refresh() {
    config::ChannelConfig cc{};
    cc.protocol = led::Protocol::Off;
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 60, led::kMaxSamplesPerFrame), 1024);
    cc.protocol = led::Protocol::WS2815;
    // refresh 0 ⇒ only the buffer constrains ⇒ the absolute cap.
    EXPECT_EQ(max_pixels_for(cc, led::kPclkHz, 0, led::kMaxSamplesPerFrame), 1024);
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

static void test_preview_pattern_colors() {
    uint8_t buf[64 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 25, 25, 3);  // lit == emit: no erase tail

    // LED 1: green base (R=0, G=26, B=0)
    EXPECT_EQ(buf[0], kPreviewGreen.r);
    EXPECT_EQ(buf[1], kPreviewGreen.g);
    EXPECT_EQ(buf[2], kPreviewGreen.b);
    // LED 10 and 20: yellow decade marks (R=G=77, B=0)
    EXPECT_EQ(buf[9 * 3 + 0], kPreviewYellow.r);
    EXPECT_EQ(buf[9 * 3 + 1], kPreviewYellow.g);
    EXPECT_EQ(buf[9 * 3 + 2], kPreviewYellow.b);
    EXPECT_EQ(buf[19 * 3 + 0], kPreviewYellow.r);
    // LED 25 (the count): white
    EXPECT_EQ(buf[24 * 3 + 0], kPreviewWhite.r);
    EXPECT_EQ(buf[24 * 3 + 1], kPreviewWhite.g);
    EXPECT_EQ(buf[24 * 3 + 2], kPreviewWhite.b);
    // LED 24: plain green base
    EXPECT_EQ(buf[23 * 3 + 1], kPreviewGreen.g);
    EXPECT_EQ(buf[23 * 3 + 0], 0);
    // Nothing written past LED 25
    EXPECT_EQ(buf[25 * 3], 0);
}

static void test_preview_pattern_centade_pink_over_decade() {
    // N = 120: LED 100 is a centade (pink, overrides the decade yellow),
    // LED 110 stays a decade, LED 120 is the count (white).
    uint8_t buf[120 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 120, 120, 3);
    EXPECT_EQ(buf[99 * 3 + 0], kPreviewPink.r);
    EXPECT_EQ(buf[99 * 3 + 1], kPreviewPink.g);
    EXPECT_EQ(buf[99 * 3 + 2], kPreviewPink.b);
    EXPECT_EQ(buf[109 * 3 + 0], kPreviewYellow.r);
    EXPECT_EQ(buf[109 * 3 + 1], kPreviewYellow.g);
    EXPECT_EQ(buf[119 * 3 + 0], kPreviewWhite.r);
    EXPECT_EQ(buf[119 * 3 + 2], kPreviewWhite.b);
}

static void test_preview_pattern_last_led_wins_over_marks() {
    // N = 30: LED 30 is both a decade mark and the count — white wins.
    uint8_t buf[32 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 30, 30, 3);
    EXPECT_EQ(buf[29 * 3 + 0], kPreviewWhite.r);
    EXPECT_EQ(buf[29 * 3 + 1], kPreviewWhite.g);
    EXPECT_EQ(buf[29 * 3 + 2], kPreviewWhite.b);
    EXPECT_EQ(buf[19 * 3 + 0], kPreviewYellow.r);  // LED 20 still a decade
}

static void test_preview_pattern_erase_tail() {
    // lit = 5, emit = 8: LEDs 6,7,8 are the dropped tail → blacked out even
    // though the buffer held stale data.
    uint8_t buf[8 * 3];
    for (auto& b : buf)
        b = 0xAB;
    fill_preview_pattern(buf, sizeof(buf), 5, 8, 3);
    EXPECT_EQ(buf[4 * 3 + 0], kPreviewWhite.r);  // LED 5: the count
    for (int led = 5; led < 8; ++led) {          // LEDs 6..8: erased
        EXPECT_EQ(buf[led * 3 + 0], 0);
        EXPECT_EQ(buf[led * 3 + 1], 0);
        EXPECT_EQ(buf[led * 3 + 2], 0);
    }
}

static void test_preview_pattern_rgbw_white_off() {
    uint8_t buf[10 * 4] = {};
    fill_preview_pattern(buf, sizeof(buf), 10, 10, 4);
    EXPECT_EQ(buf[1], kPreviewGreen.g);  // LED 1 green
    EXPECT_EQ(buf[3], 0);                // W byte stays dark
    EXPECT_EQ(buf[9 * 4 + 0], kPreviewWhite.r);
    EXPECT_EQ(buf[9 * 4 + 3], 0);  // LED 10 white but W still dark
}

static void test_preview_pattern_single_pixel() {
    uint8_t buf[3] = {};
    fill_preview_pattern(buf, sizeof(buf), 1, 1, 3);
    EXPECT_EQ(buf[0], kPreviewWhite.r);  // sole LED is the count → white
    EXPECT_EQ(buf[1], kPreviewWhite.g);
    EXPECT_EQ(buf[2], kPreviewWhite.b);
}

static void test_preview_pattern_overflow_is_noop() {
    uint8_t buf[3 * 3] = {};
    fill_preview_pattern(buf, sizeof(buf), 4, 4, 3);  // 12 bytes > 9-byte buffer
    EXPECT_EQ(buf[0], 0);
    // The emit count (not the lit count) drives the capacity guard.
    uint8_t buf2[5 * 3] = {};
    fill_preview_pattern(buf2, sizeof(buf2), 3, 6, 3);  // emit 6 → 18 > 15 bytes
    EXPECT_EQ(buf2[0], 0);
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

// ── Scene generators ────────────────────────────────────────────────────────

static pixfrog::config::Scene mk_scene(uint8_t effect, uint8_t r, uint8_t g, uint8_t b,
                                       uint8_t speed, uint8_t param) {
    pixfrog::config::Scene s{};
    s.effect     = effect;
    s.r          = r;
    s.g          = g;
    s.b          = b;
    s.speed      = speed;
    s.param      = param;
    s.num_colors = 1;
    return s;
}

static pixfrog::config::Scene with_color(pixfrog::config::Scene s, uint8_t r, uint8_t g,
                                         uint8_t b) {
    pixfrog::config::set_scene_color(s, s.num_colors, r, g, b);
    ++s.num_colors;
    return s;
}

static void test_scene_solid() {
    uint8_t buf[4 * 3] = {};
    fill_scene_pattern(buf, sizeof(buf), 4, 3, mk_scene(0 /*solid*/, 10, 20, 30, 0, 0), 12345);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(buf[i * 3 + 0], 10);
        EXPECT_EQ(buf[i * 3 + 1], 20);
        EXPECT_EQ(buf[i * 3 + 2], 30);
    }
}

static void test_scene_solid_rgbw_white_off() {
    uint8_t buf[2 * 4];
    std::memset(buf, 0xFF, sizeof(buf));
    fill_scene_pattern(buf, sizeof(buf), 2, 4, mk_scene(0, 1, 2, 3, 0, 0), 0);
    EXPECT_EQ(buf[3], 0);
    EXPECT_EQ(buf[7], 0);
}

static void test_scene_chase_position_and_width() {
    // 10 px, speed 100 px/s, t=0 → head at 0; width 3 → pixels 0, 9, 8 lit.
    uint8_t buf[10 * 3] = {};
    fill_scene_pattern(buf, sizeof(buf), 10, 3, mk_scene(1 /*chase*/, 255, 0, 0, 100, 3), 0);
    EXPECT_EQ(buf[0 * 3], 255);
    EXPECT_EQ(buf[9 * 3], 255);
    EXPECT_EQ(buf[8 * 3], 255);
    EXPECT_EQ(buf[5 * 3], 0);  // background black

    // t=1000 ms → 100 px advanced → head back at 0 (wrap).
    uint8_t buf2[10 * 3] = {};
    fill_scene_pattern(buf2, sizeof(buf2), 10, 3, mk_scene(1, 255, 0, 0, 100, 3), 1000);
    EXPECT_EQ(buf2[0 * 3], 255);

    // t=50 ms → 5 px advanced → head at 5.
    uint8_t buf3[10 * 3] = {};
    fill_scene_pattern(buf3, sizeof(buf3), 10, 3, mk_scene(1, 255, 0, 0, 100, 3), 50);
    EXPECT_EQ(buf3[5 * 3], 255);
    EXPECT_EQ(buf3[0 * 3], 0);
}

static void test_scene_rainbow_spans_hues() {
    // 6 px, 1 repeat, t=0 → hues 0,60,...,300 → all distinct primaries/mixes.
    uint8_t buf[6 * 3] = {};
    fill_scene_pattern(buf, sizeof(buf), 6, 3, mk_scene(2 /*rainbow*/, 0, 0, 0, 0, 1), 0);
    EXPECT_EQ(buf[0], 255);  // hue 0 = red
    EXPECT_EQ(buf[1], 0);
    // hue 120 (pixel 2) = green
    EXPECT_EQ(buf[2 * 3 + 0], 0);
    EXPECT_EQ(buf[2 * 3 + 1], 255);
    // hue 240 (pixel 4) = blue
    EXPECT_EQ(buf[4 * 3 + 2], 255);
    // rotation: with speed, t shifts the wheel
    uint8_t buf2[6 * 3] = {};
    fill_scene_pattern(buf2, sizeof(buf2), 6, 3, mk_scene(2, 0, 0, 0, 100, 1), 60);  // +60°
    EXPECT_EQ(buf2[0 * 3 + 0], 255);  // hue 60 = yellow
    EXPECT_EQ(buf2[0 * 3 + 1], 255);
}

static void test_scene_overflow_is_noop() {
    uint8_t buf[5] = {};
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mk_scene(0, 9, 9, 9, 0, 0), 0);
    EXPECT_EQ(buf[0], 0);
}

// Strobe: 0 → steady colour 1, 255 → steady colour 2, in between 1/60 s
// flashes of colour 2 at speed×60/255 Hz.
static void test_scene_solid_strobe() {
    using namespace pixfrog::config;
    const Scene base = with_color(mk_scene(kSceneFxSolid, 10, 0, 0, 0, 0), 0, 0, 200);
    uint8_t buf[2 * 3];
    for (uint32_t t : { 0u, 7u, 999u, 123456u }) {
        fill_scene_pattern(buf, sizeof(buf), 2, 3, base, t);
        EXPECT_EQ(buf[0], 10);  // speed 0: never flashes
        EXPECT_EQ(buf[2], 0);
    }
    Scene full = base;
    full.speed = 255;
    for (uint32_t t : { 0u, 7u, 999u, 123456u }) {
        fill_scene_pattern(buf, sizeof(buf), 2, 3, full, t);
        EXPECT_EQ(buf[0], 0);  // 60 Hz of 1/60 s flashes: always colour 2
        EXPECT_EQ(buf[2], 200);
    }
    // 51 → 12 Hz: period 83.3 ms, flash for the first 16.7 ms of each.
    Scene mid = base;
    mid.speed = 51;
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 5);
    EXPECT_EQ(buf[2], 200);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 40);
    EXPECT_EQ(buf[0], 10);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 90);  // next period's flash
    EXPECT_EQ(buf[2], 200);
    // One colour: the flash is black (classic strobe on a lit strip).
    Scene single = mk_scene(kSceneFxSolid, 50, 50, 50, 255, 0);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, single, 3);
    EXPECT_EQ(buf[0], 0);
}

static void test_scene_chase_one_head_per_colour() {
    using namespace pixfrog::config;
    const Scene s       = with_color(mk_scene(kSceneFxChase, 255, 0, 0, 0, 1), 0, 0, 255);
    uint8_t buf[10 * 3] = {};
    fill_scene_pattern(buf, sizeof(buf), 10, 3, s, 0);
    EXPECT_EQ(buf[0 * 3 + 0], 255);  // head 1 at 0
    EXPECT_EQ(buf[5 * 3 + 2], 255);  // head 2 half a strip away
    EXPECT_EQ(buf[3 * 3 + 0], 0);
}

static int lit_pixels(const uint8_t* buf, int n) {
    int lit = 0;
    for (int i = 0; i < n; ++i)
        if (buf[i * 3] | buf[i * 3 + 1] | buf[i * 3 + 2]) ++lit;
    return lit;
}

static void test_scene_blobs_move_and_stay_in_bounds() {
    using namespace pixfrog::config;
    const Scene s    = with_color(mk_scene(kSceneFxBlobs, 0, 0, 255, 60, 3), 255, 0, 0);
    constexpr int kN = 120;
    uint8_t a[kN * 3 + 3], b[kN * 3 + 3];
    std::memset(a, 0xAB, sizeof(a));
    std::memset(b, 0xAB, sizeof(b));
    fill_scene_pattern(a, sizeof(a), kN, 3, s, 1000);
    fill_scene_pattern(b, sizeof(b), kN, 3, s, 3000);
    EXPECT_EQ(a[kN * 3], 0xAB);  // nothing past the strip
    EXPECT_TRUE(lit_pixels(a, kN) > 10);
    EXPECT_TRUE(lit_pixels(a, kN) < kN);  // blobs, not a wash
    EXPECT_TRUE(std::memcmp(a, b, kN * 3) != 0);
    // Deterministic: same time, same frame.
    fill_scene_pattern(b, sizeof(b), kN, 3, s, 1000);
    EXPECT_TRUE(std::memcmp(a, b, kN * 3) == 0);
}

static void test_scene_fire_hot_base_dark_tip() {
    using namespace pixfrog::config;
    Scene s          = with_color(mk_scene(kSceneFxFire, 255, 0, 0, 60, 0), 255, 255, 0);
    constexpr int kN = 60;
    uint8_t buf[kN * 3];
    fill_scene_pattern(buf, sizeof(buf), kN, 3, s, 4321);
    EXPECT_TRUE(buf[0] > 100);            // base glows
    EXPECT_EQ(buf[(kN - 1) * 3 + 1], 0);  // tip burns out
}

static void test_scene_scanner_bounces() {
    using namespace pixfrog::config;
    const Scene s = mk_scene(kSceneFxScanner, 0, 255, 0, 100, 1);
    uint8_t buf[11 * 3];
    fill_scene_pattern(buf, sizeof(buf), 11, 3, s, 50);  // 5 px along the first leg
    EXPECT_EQ(buf[5 * 3 + 1], 255);
    EXPECT_TRUE(buf[4 * 3 + 1] > 0);                      // trail behind
    EXPECT_EQ(buf[6 * 3 + 1], 0);                         // nothing ahead
    fill_scene_pattern(buf, sizeof(buf), 11, 3, s, 150);  // 15 px: bounced back to 5
    EXPECT_EQ(buf[5 * 3 + 1], 255);
    EXPECT_TRUE(buf[6 * 3 + 1] > 0);  // trail now on the other side
}

static void test_scene_stripes_alternate_palette() {
    using namespace pixfrog::config;
    const Scene s = with_color(mk_scene(kSceneFxStripes, 255, 0, 0, 0, 2), 0, 255, 0);
    uint8_t buf[8 * 3];
    fill_scene_pattern(buf, sizeof(buf), 8, 3, s, 0);
    EXPECT_EQ(buf[0 * 3 + 0], 255);
    EXPECT_EQ(buf[1 * 3 + 0], 255);
    EXPECT_EQ(buf[2 * 3 + 1], 255);
    EXPECT_EQ(buf[4 * 3 + 0], 255);
}

static void test_scene_fade_crosses_palette() {
    using namespace pixfrog::config;
    const Scene s = with_color(mk_scene(kSceneFxFade, 255, 0, 0, 255, 0), 0, 0, 255);
    uint8_t buf[3];
    fill_scene_pattern(buf, sizeof(buf), 1, 3, s, 0);
    EXPECT_EQ(buf[0], 255);
    EXPECT_EQ(buf[2], 0);
    // 255 × 256 / 1000 per ms: half a cycle (colour 2) after ~502 ms.
    fill_scene_pattern(buf, sizeof(buf), 1, 3, s, 502);
    EXPECT_TRUE(buf[2] > 240);
    EXPECT_TRUE(buf[0] < 15);
}

// Every effect must stay within pixel_count × bpp, zero the W die, and cope
// with 1-pixel strips, max-size strips and extreme parameters.
static void test_scene_all_effects_bounded() {
    using namespace pixfrog::config;
    constexpr int kN = 1024;
    static uint8_t buf[kN * 4 + 8];
    for (uint8_t fx = 0; fx < kSceneFxCount; ++fx)
        for (int n : { 1, 2, 7, kN })
            for (uint8_t speed : { 0, 1, 255 })
                for (uint8_t param : { 0, 1, 255 }) {
                    Scene s = with_color(with_color(mk_scene(fx, 255, 1, 2, speed, param), 3, 4, 5),
                                         6, 7, 8);
                    s       = with_color(s, 9, 10, 11);
                    std::memset(buf, 0xCD, sizeof(buf));
                    fill_scene_pattern(buf, sizeof(buf), static_cast<uint16_t>(n), 4, s,
                                       0xFFFFFFF0u);
                    bool ok = buf[n * 4] == 0xCD;
                    for (int i = 0; i < n; ++i)
                        ok = ok && buf[i * 4 + 3] == 0;
                    EXPECT_TRUE(ok);
                }
}

static void test_hue_wheel_endpoints() {
    uint8_t r, g, b;
    hue_to_rgb(0, &r, &g, &b);
    EXPECT_EQ(r, 255);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
    hue_to_rgb(120, &r, &g, &b);
    EXPECT_EQ(g, 255);
    EXPECT_EQ(r, 0);
    hue_to_rgb(240, &r, &g, &b);
    EXPECT_EQ(b, 255);
    EXPECT_EQ(g, 0);
    hue_to_rgb(359, &r, &g, &b);
    EXPECT_EQ(r, 255);            // wraps back toward red
    hue_to_rgb(720, &r, &g, &b);  // modulo
    EXPECT_EQ(r, 255);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
}

// ── 2-source merge (HTP/LTP) ────────────────────────────────────────────────

constexpr int64_t kTestTimeoutUs = 10'000'000;

static void test_merge_single_source_passthrough() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t frame[4]                   = { 10, 20, 30, 40 };

    EXPECT_TRUE(merge_ingest(m, staging, dst, frame, sizeof(frame), 0xA1, false, 1'000'000,
                             kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 1);
    EXPECT_EQ(dst[0], 10);
    EXPECT_EQ(dst[3], 40);
}

static void test_merge_htp_two_sources() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t a[3]                       = { 100, 5, 200 };
    uint8_t b[3]                       = { 50, 60, 250 };

    EXPECT_TRUE(
        merge_ingest(m, staging, dst, a, sizeof(a), 0xA1, false, 1'000'000, kTestTimeoutUs));
    EXPECT_TRUE(
        merge_ingest(m, staging, dst, b, sizeof(b), 0xB2, false, 1'100'000, kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 2);
    EXPECT_EQ(dst[0], 100);  // max(100, 50)
    EXPECT_EQ(dst[1], 60);   // max(5, 60)
    EXPECT_EQ(dst[2], 250);  // max(200, 250)
}

static void test_merge_ltp_last_frame_wins() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t a[3]                       = { 100, 100, 100 };
    uint8_t b[3]                       = { 1, 2, 3 };

    EXPECT_TRUE(merge_ingest(m, staging, dst, a, sizeof(a), 0xA1, true, 1'000'000, kTestTimeoutUs));
    EXPECT_TRUE(merge_ingest(m, staging, dst, b, sizeof(b), 0xB2, true, 1'100'000, kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 2);  // both tracked (for reporting)…
    EXPECT_EQ(dst[0], 1);                 // …but the last frame wins
    EXPECT_EQ(dst[1], 2);
    EXPECT_EQ(dst[2], 3);
}

static void test_merge_third_source_rejected() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t frame[1]                   = { 9 };

    EXPECT_TRUE(merge_ingest(m, staging, dst, frame, 1, 0xA1, false, 1'000'000, kTestTimeoutUs));
    EXPECT_TRUE(merge_ingest(m, staging, dst, frame, 1, 0xB2, false, 1'000'000, kTestTimeoutUs));
    EXPECT_TRUE(!merge_ingest(m, staging, dst, frame, 1, 0xC3, false, 1'000'000, kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 2);
}

static void test_merge_source_timeout_drops() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t a[2]                       = { 100, 100 };
    uint8_t b[2]                       = { 10, 10 };

    merge_ingest(m, staging, dst, a, 2, 0xA1, false, 1'000'000, kTestTimeoutUs);
    merge_ingest(m, staging, dst, b, 2, 0xB2, false, 1'000'000, kTestTimeoutUs);
    EXPECT_EQ(dst[0], 100);  // HTP while both live

    // A goes silent past the timeout; B's next frame is exclusive again.
    const int64_t later = 1'000'000 + kTestTimeoutUs + 1;
    EXPECT_TRUE(merge_ingest(m, staging, dst, b, 2, 0xB2, false, later, kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 1);
    EXPECT_EQ(dst[0], 10);

    // …and a third sender can claim the freed slot.
    uint8_t c[2] = { 77, 77 };
    EXPECT_TRUE(merge_ingest(m, staging, dst, c, 2, 0xC3, false, later + 1, kTestTimeoutUs));
    EXPECT_EQ(merge_active_count(m), 2);
}

static void test_merge_fresh_claim_zeroes_staging() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize];
    std::memset(staging, 0xEE, sizeof(staging));  // dirt from a previous occupant
    uint8_t dst[kUniverseSize] = {};
    uint8_t a[8]               = { 10, 10, 10, 10, 10, 10, 10, 10 };
    uint8_t b[2]               = { 200, 200 };  // shorter frame

    merge_ingest(m, staging, dst, a, sizeof(a), 0xA1, false, 1'000'000, kTestTimeoutUs);
    merge_ingest(m, staging, dst, b, sizeof(b), 0xB2, false, 1'100'000, kTestTimeoutUs);
    EXPECT_EQ(dst[0], 200);  // max(10, 200)
    EXPECT_EQ(dst[2], 10);   // B's tail is zero, not 0xEE
    EXPECT_EQ(dst[7], 10);
    EXPECT_EQ(dst[8], 0);  // beyond both frames: zero, no dirt
}

static void test_merge_drop_and_claim_refresh() {
    MergeState m{};
    bool fresh = false;
    EXPECT_EQ(merge_claim(m, 0xA1, 1'000'000, &fresh), 0);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(merge_claim(m, 0xA1, 2'000'000, &fresh), 0);  // refresh, same slot
    EXPECT_TRUE(!fresh);
    EXPECT_EQ(m.last_us[0], 2'000'000);

    merge_drop(m, 0xA1);
    EXPECT_EQ(merge_active_count(m), 0);
    merge_drop(m, 0xDEAD);  // dropping an unknown id is a no-op
}

static void test_merge_zero_id_coerced() {
    MergeState m{};
    // id 0 (the free-slot sentinel) is coerced to 1, so it still tracks.
    EXPECT_EQ(merge_claim(m, 0, 1'000'000, nullptr), 0);
    EXPECT_EQ(merge_active_count(m), 1);
    EXPECT_EQ(merge_claim(m, 1, 1'000'001, nullptr), 0);  // same source as id 0
    EXPECT_EQ(merge_active_count(m), 1);
}

static void test_merge_htp_full_universe() {
    MergeState m{};
    uint8_t staging[2 * kUniverseSize] = {};
    uint8_t dst[kUniverseSize]         = {};
    uint8_t a[kUniverseSize], b[kUniverseSize];
    for (size_t i = 0; i < kUniverseSize; ++i) {
        a[i] = static_cast<uint8_t>(i);
        b[i] = static_cast<uint8_t>(255 - i);
    }
    merge_ingest(m, staging, dst, a, sizeof(a), 0xA1, false, 1'000'000, kTestTimeoutUs);
    merge_ingest(m, staging, dst, b, sizeof(b), 0xB2, false, 1'000'001, kTestTimeoutUs);
    for (size_t i = 0; i < kUniverseSize; ++i)
        EXPECT_EQ(dst[i], a[i] > b[i] ? a[i] : b[i]);
}

int main() {
    test_total_bytes_rgb();
    test_total_bytes_rgbw();
    test_universes_used();
    test_universes_off();
    test_auto_patch_cascade();
    test_t_dma_ws2815();
    test_emission_budget();
    test_capacity_check();
    test_capacity_dmx_ignores_led_refresh();
    test_max_pixels_ws2815();
    test_max_pixels_dmx();
    test_max_pixels_off_and_zero_refresh();
    test_decode_single_universe();
    test_decode_dmx_start_offset();
    test_decode_multi_universe();
    test_decode_missing_universe();
    test_decode_dst_too_small();
    test_decode_dmx_start_in_second_universe();
    test_preview_pattern_colors();
    test_preview_pattern_centade_pink_over_decade();
    test_preview_pattern_last_led_wins_over_marks();
    test_preview_pattern_erase_tail();
    test_preview_pattern_rgbw_white_off();
    test_preview_pattern_single_pixel();
    test_preview_pattern_overflow_is_noop();
    test_failsafe_due_logic();
    test_failsafe_fill_blackout();
    test_failsafe_fill_color_rgb();
    test_failsafe_fill_color_rgbw_white_off();
    test_failsafe_fill_overflow_is_noop();
    test_scene_solid();
    test_scene_solid_rgbw_white_off();
    test_scene_chase_position_and_width();
    test_scene_rainbow_spans_hues();
    test_scene_overflow_is_noop();
    test_scene_solid_strobe();
    test_scene_chase_one_head_per_colour();
    test_scene_blobs_move_and_stay_in_bounds();
    test_scene_fire_hot_base_dark_tip();
    test_scene_scanner_bounces();
    test_scene_stripes_alternate_palette();
    test_scene_fade_crosses_palette();
    test_scene_all_effects_bounded();
    test_hue_wheel_endpoints();
    test_merge_single_source_passthrough();
    test_merge_htp_two_sources();
    test_merge_ltp_last_frame_wins();
    test_merge_third_source_rejected();
    test_merge_source_timeout_drops();
    test_merge_fresh_claim_zeroes_staging();
    test_merge_drop_and_claim_refresh();
    test_merge_zero_id_coerced();
    test_merge_htp_full_universe();
    test_universe_map_basic();
    test_universe_map_rgbw_fills_pool();
    test_universe_map_pool_exhaustion_is_reported();
    test_universe_map_clamps_at_top_of_range();
    test_universe_routable_range();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
