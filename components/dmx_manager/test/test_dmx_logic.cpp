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

// The generators are exercised in their own speed units, through the v3 scene
// record that carried a look before the effect bank.
static void fill_scene_pattern(uint8_t* dst, size_t cap, uint16_t n, uint8_t bpp,
                               const config::SceneV3& s, uint64_t t) {
    config::Effect e = config::effect_from_scene_v3(s);
    fill_generator(dst, cap, n, bpp, e.generator, effect_palette(e), s.speed, s.param, t);
}

static void fill_scene_on_channel(uint8_t* dst, size_t cap, const config::ChannelConfig& cc,
                                  uint8_t bpp, const config::SceneV3& s, uint64_t t) {
    fill_on_channel(dst, cap, cc, bpp, s.fixture_mode, [&](uint8_t* d, size_t c, uint16_t n) {
        fill_scene_pattern(d, c, n, bpp, s, t);
    });
}

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

    uint16_t out[8]{}, dmx[8]{};
    AutoPatchOptions o;
    o.base              = 14;
    const uint16_t next = compute_auto_patch(o, chans, 8, out, dmx);
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

// ── DMX layout (packing) ────────────────────────────────────────────────────

static config::ChannelConfig rgb_chan(uint16_t px, uint8_t packing, uint16_t dmx_start = 1) {
    config::ChannelConfig c{};
    c.protocol       = led::Protocol::WS2815;
    c.pixel_count    = px;
    c.grouping       = 1;
    c.universe_start = 1;
    c.dmx_start      = dmx_start;
    c.packing        = packing;
    return c;
}

static void test_layout_whole_pixels_never_split_one() {
    DmxRun r[kMaxDmxRuns];
    auto c         = rgb_chan(1024, config::kPackWholePixels);
    const size_t n = channel_layout(c, r, kMaxDmxRuns);
    EXPECT_EQ(n, 7u);            // 170 px a universe: 7, not 6
    EXPECT_EQ(r[0].bytes, 510);  // two slots left unused
    EXPECT_EQ(r[1].uni_off, 1);
    EXPECT_EQ(r[1].slot, 0);
    EXPECT_EQ(r[1].dst, 510);  // pixel 171 opens universe 2
    EXPECT_EQ(channel_universes_used(c), 7u);
    c.packing = config::kPackContinuous;
    EXPECT_EQ(channel_universes_used(c), 6u);  // byte after byte
    c.protocol = led::Protocol::SK6812;        // RGBW: 128 px fit exactly
    c.packing  = config::kPackWholePixels;
    EXPECT_EQ(channel_universes_used(c), 8u);
    // Not one whole pixel left after dmx_start 511: it starts in the next one.
    auto d = rgb_chan(10, config::kPackWholePixels, 511);
    EXPECT_EQ(channel_layout(d, r, kMaxDmxRuns), 1u);
    EXPECT_EQ(r[0].uni_off, 1);
    EXPECT_EQ(r[0].slot, 0);
}

// The span used to ignore dmx_start: 300 bytes from slot 400 need 2 universes.
static void test_layout_counts_the_start_address() {
    auto c = rgb_chan(100, config::kPackContinuous, 401);
    EXPECT_EQ(channel_universes_used(c), 2u);
    c.dmx_start = 1;
    EXPECT_EQ(channel_universes_used(c), 1u);
}

static void test_layout_one_fixture_per_universe() {
    auto c = rgb_chan(295, config::kPackPerFixture);  // 5 bars of 59, a dead LED between
    for (uint16_t k = 0; k < 5; ++k)
        c.fixtures[k] = { static_cast<uint16_t>(k * 60), 59 };
    for (uint16_t k = 0; k < 4; ++k)
        c.gaps[k] = { static_cast<uint16_t>(59 + k * 60), 1 };
    DmxRun r[kMaxDmxRuns];
    EXPECT_EQ(channel_layout(c, r, kMaxDmxRuns), 5u);
    for (uint16_t k = 0; k < 5; ++k) {
        EXPECT_EQ(r[k].uni_off, k);  // each bar from slot 1 of its own universe
        EXPECT_EQ(r[k].slot, 0);
        EXPECT_EQ(r[k].dst, k * 177);
        EXPECT_EQ(r[k].bytes, 177);
    }
    // A 200-pixel fixture runs into a second universe (whole pixels), the next
    // fixture still opens its own.
    auto big        = rgb_chan(300, config::kPackPerFixture);
    big.fixtures[0] = { 0, 200 };
    big.fixtures[1] = { 200, 100 };
    EXPECT_EQ(channel_layout(big, r, kMaxDmxRuns), 3u);
    EXPECT_EQ(r[1].uni_off, 1);
    EXPECT_EQ(r[1].bytes, 90);  // pixels 171-200
    EXPECT_EQ(r[2].uni_off, 2);
    EXPECT_EQ(r[2].dst, 600);
    // No fixtures: whole pixels.
    auto none = rgb_chan(200, config::kPackPerFixture);
    EXPECT_EQ(channel_universes_used(none), 2u);
    EXPECT_EQ(channel_layout(none, r, kMaxDmxRuns), 2u);
    EXPECT_EQ(r[0].bytes, 510);
}

static void test_decode_per_fixture_leaves_the_rest_dark() {
    auto c        = rgb_chan(10, config::kPackPerFixture);
    c.fixtures[0] = { 0, 3 };
    c.fixtures[1] = { 6, 2 };  // pixels 3-5 and 8-9 are in no fixture
    uint8_t u1[512], u2[512];
    std::memset(u1, 0x11, sizeof(u1));
    std::memset(u2, 0x22, sizeof(u2));
    auto get = [&](uint16_t u) -> const uint8_t* { return u == 1 ? u1 : u == 2 ? u2 : nullptr; };
    uint8_t px[30];
    std::memset(px, 0xEE, sizeof(px));
    EXPECT_TRUE(decode_pixels(px, sizeof(px), c, get));
    EXPECT_EQ(px[0], 0x11);
    EXPECT_EQ(px[3 * 3], 0);     // no fixture: no data
    EXPECT_EQ(px[6 * 3], 0x22);  // fixture 2 from universe 2
    EXPECT_EQ(px[9 * 3], 0);
    // A missing universe darkens its run only and is reported.
    auto only1 = [&](uint16_t u) -> const uint8_t* { return u == 1 ? u1 : nullptr; };
    EXPECT_TRUE(!decode_pixels(px, sizeof(px), c, only1));
    EXPECT_EQ(px[0], 0x11);
    EXPECT_EQ(px[6 * 3], 0);
}

static void test_auto_patch_compact_and_forced_packing() {
    config::ChannelConfig chans[8];
    for (auto& c : chans)
        c = rgb_chan(50, config::kPackContinuous);  // 150 B each
    uint16_t uni[8], dmx[8], slot_after = 0;
    AutoPatchOptions o;
    o.compact           = true;
    const uint16_t next = compute_auto_patch(o, chans, 8, uni, dmx, &slot_after);
    for (uint16_t i = 0; i < 8; ++i) {  // one after the other, byte after byte
        EXPECT_EQ(uni[i], 150 * i / 512);
        EXPECT_EQ(dmx[i], 150 * i % 512 + 1);
    }
    EXPECT_EQ(next, 3);  // 1200 B: universes 0-2
    EXPECT_EQ(slot_after, 1200 % 512);
    // Whole pixels, compact: channel 3 starts at 451, 20 px fit before 512.
    o.packing = config::kPackWholePixels;
    compute_auto_patch(o, chans, 8, uni, dmx);
    EXPECT_EQ(uni[3], 0);
    EXPECT_EQ(dmx[3], 451);
    EXPECT_EQ(chans[3].packing, config::kPackWholePixels);  // applied to every channel
    // Per fixture always opens a universe, even compact.
    o.packing = config::kPackPerFixture;
    compute_auto_patch(o, chans, 8, uni, dmx);
    EXPECT_EQ(uni[1], 1);
    EXPECT_EQ(dmx[1], 1);
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

// Compact patching: two outputs share a universe — one slot, both bits.
static void test_universe_map_shares_a_universe() {
    config::ChannelConfig chans[2] = { rgb_chan(50, 0), rgb_chan(50, 0, 151) };
    UniMap m(8);
    m.build(chans, 2);
    EXPECT_EQ(m.used, 1);
    EXPECT_EQ(m.unmapped, 0u);
    EXPECT_EQ(m.slot_to_chan[0], 0x3);
}

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
    EXPECT_EQ(m.slot_to_chan[7], 1u << 7);  // a bit per channel fed
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
    EXPECT_EQ(m.slot_to_chan[63], 1u << 7);
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

// ── SceneV3 generators ────────────────────────────────────────────────────────

static pixfrog::config::SceneV3 mk_scene(uint8_t effect, uint8_t r, uint8_t g, uint8_t b,
                                         uint8_t speed, uint8_t param) {
    pixfrog::config::SceneV3 s{};
    s.effect     = effect;
    s.r          = r;
    s.g          = g;
    s.b          = b;
    s.speed      = speed;
    s.param      = param;
    s.num_colors = 1;
    return s;
}

static pixfrog::config::SceneV3 with_color(pixfrog::config::SceneV3 s, uint8_t r, uint8_t g,
                                           uint8_t b) {
    pixfrog::config::set_scene_color(s, s.num_colors, r, g, b);
    ++s.num_colors;
    return s;
}

// ── Fixtures ────────────────────────────────────────────────────────────────

static pixfrog::config::ChannelConfig fixture_chan(uint16_t live) {
    pixfrog::config::ChannelConfig cc{};
    cc.protocol    = pixfrog::led::Protocol::WS2815;
    cc.pixel_count = live;
    cc.grouping    = 1;
    return cc;
}

// The bench example: 5 bars of 59 LEDs, one dead LED between two bars.
static void test_fixture_spans_skip_the_dead_leds() {
    auto cc = fixture_chan(295);
    for (uint16_t k = 0; k < 5; ++k)
        cc.fixtures[k] = { static_cast<uint16_t>(k * 60), 59 };
    for (uint16_t k = 0; k < 4; ++k)
        cc.gaps[k] = { static_cast<uint16_t>(59 + k * 60), 1 };
    Span sp[pixfrog::config::kMaxFixtures];
    EXPECT_EQ(fixture_spans(cc, sp, pixfrog::config::kMaxFixtures), 5u);
    for (uint16_t k = 0; k < 5; ++k) {
        EXPECT_EQ(sp[k].first, k * 59);
        EXPECT_EQ(sp[k].count, 59);
    }
    cc.pixel_count = 200;  // the strip now ends inside bar 4: cut, bar 5 dropped
    EXPECT_EQ(fixture_spans(cc, sp, pixfrog::config::kMaxFixtures), 4u);
    EXPECT_EQ(sp[3].count, 200 - 3 * 59);
}

static void test_fixture_spans_follow_invert_and_grouping() {
    auto cc        = fixture_chan(15);  // A: 0-9, dead 10-11, B: 12-16
    cc.fixtures[0] = { 0, 10 };
    cc.fixtures[1] = { 12, 5 };
    cc.gaps[0]     = { 10, 2 };
    Span sp[4];
    cc.invert_direction = true;  // B comes first in the buffer
    EXPECT_EQ(fixture_spans(cc, sp, 4), 2u);
    EXPECT_EQ(sp[0].first, 0);
    EXPECT_EQ(sp[0].count, 5);
    EXPECT_EQ(sp[1].first, 5);
    EXPECT_EQ(sp[1].count, 10);
    cc.invert_direction = false;
    cc.grouping         = 2;  // two LEDs per buffer pixel
    EXPECT_EQ(fixture_spans(cc, sp, 4), 2u);
    EXPECT_EQ(sp[0].count, 5);
    EXPECT_EQ(sp[1].first, 5);
    EXPECT_EQ(sp[1].count, 3);
}

static void test_fixture_modes_each_chain_mirror() {
    auto cc        = fixture_chan(12);  // 0-3 and 8-11; 4-7 are no fixture
    cc.fixtures[0] = { 0, 4 };
    cc.fixtures[1] = { 8, 4 };
    auto sc        = with_color(mk_scene(4 /*gradient*/, 255, 0, 0, 0, 1), 0, 0, 255);
    uint8_t buf[12 * 3], ref[12 * 3];
    // Strip: the plain pattern, fixtures ignored.
    sc.fixture_mode = pixfrog::config::kFixtureModeStrip;
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    fill_scene_pattern(ref, sizeof(ref), 12, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf, ref, sizeof(buf)) == 0);
    // Each: both fixtures show the same 4-pixel pattern, dark between.
    sc.fixture_mode = pixfrog::config::kFixtureModeEach;
    std::memset(buf, 0xEE, sizeof(buf));
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    fill_scene_pattern(ref, sizeof(ref), 4, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf, ref, 4 * 3) == 0);
    EXPECT_TRUE(std::memcmp(buf + 8 * 3, ref, 4 * 3) == 0);
    for (int i = 4 * 3; i < 8 * 3; ++i)
        EXPECT_EQ(buf[i], 0);
    // Chain: one 8-pixel pattern, its second half on the second fixture.
    sc.fixture_mode = pixfrog::config::kFixtureModeChain;
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    fill_scene_pattern(ref, sizeof(ref), 8, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf, ref, 4 * 3) == 0);
    EXPECT_TRUE(std::memcmp(buf + 8 * 3, ref + 4 * 3, 4 * 3) == 0);
    EXPECT_EQ(buf[5 * 3], 0);
    // Mirror over three fixtures: the third is the first reversed.
    cc.fixtures[1]  = { 4, 4 };
    cc.fixtures[2]  = { 8, 4 };
    sc.fixture_mode = pixfrog::config::kFixtureModeMirror;
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    fill_scene_pattern(ref, sizeof(ref), 8, 3, sc, 0);  // chained over fixtures 1-2
    EXPECT_TRUE(std::memcmp(buf, ref, 8 * 3) == 0);
    for (int j = 0; j < 4; ++j)
        EXPECT_TRUE(std::memcmp(buf + (8 + j) * 3, buf + (3 - j) * 3, 3) == 0);
    // Mirror with a longer partner: resampled, still reversed end to end.
    cc.fixtures[2] = { 8, 2 };
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf + 8 * 3, buf + 3 * 3, 3) == 0);
    EXPECT_TRUE(std::memcmp(buf + 9 * 3, buf + 1 * 3, 3) == 0);
    EXPECT_EQ(buf[10 * 3], 0);  // past the short fixture: dark
}

// A fixture mounted the other way round runs the effect backwards, in every
// mode but the whole strip; the flag survives the sort.
static void test_reversed_fixture_runs_backwards() {
    auto cc         = fixture_chan(8);
    cc.fixtures[0]  = { 0, 4 };
    cc.fixtures[1]  = { 4, static_cast<uint16_t>(4 | pixfrog::config::kFixtureReversed) };
    auto sc         = with_color(mk_scene(4 /*gradient*/, 255, 0, 0, 0, 1), 0, 0, 255);
    sc.fixture_mode = pixfrog::config::kFixtureModeEach;
    uint8_t buf[8 * 3];
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    for (int j = 0; j < 4; ++j)  // the second fixture is the first one flipped
        EXPECT_TRUE(std::memcmp(buf + (4 + j) * 3, buf + (3 - j) * 3, 3) == 0);
    sc.fixture_mode = pixfrog::config::kFixtureModeChain;
    uint8_t ref[8 * 3];
    fill_scene_pattern(ref, sizeof(ref), 8, 3, sc, 0);
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf, ref, 4 * 3) == 0);
    for (int j = 0; j < 4; ++j)
        EXPECT_TRUE(std::memcmp(buf + (4 + j) * 3, ref + (7 - j) * 3, 3) == 0);
    sc.fixture_mode = pixfrog::config::kFixtureModeStrip;  // fixtures ignored
    fill_scene_on_channel(buf, sizeof(buf), cc, 3, sc, 0);
    EXPECT_TRUE(std::memcmp(buf, ref, sizeof(buf)) == 0);
    pixfrog::config::Fixture f[2] = { cc.fixtures[1], cc.fixtures[0] };
    pixfrog::config::normalize_fixtures(f, 2);
    EXPECT_TRUE(pixfrog::config::fixture_reversed(f[1]));
    EXPECT_EQ(pixfrog::config::fixture_len(f[1]), 4);
}

static void test_fixtures_normalize_drops_overlaps() {
    pixfrog::config::Fixture f[4] = { { 20, 5 }, { 0, 10 }, { 5, 3 }, { 2000, 4 } };
    EXPECT_EQ(pixfrog::config::normalize_fixtures(f, 4), 2u);
    EXPECT_EQ(f[0].pos, 0);
    EXPECT_EQ(f[1].pos, 20);
    EXPECT_EQ(f[2].len, 0);
}

// The stored count survives a refresh change; only the emitted count follows
// the budget (it used to be rewritten in NVS: 1024 px @30 Hz → 512 @60 Hz for good).
static void test_effective_pixel_count_non_destructive() {
    pixfrog::config::ChannelConfig cc{};
    cc.protocol         = pixfrog::led::Protocol::WS2815;
    cc.pixel_count      = 1024;
    const size_t buf    = pixfrog::led::kMaxSamplesPerFrame;
    const uint32_t pclk = pixfrog::led::kPclkHz;
    EXPECT_EQ(effective_pixel_count(cc, pclk, 30, buf), 1024);
    EXPECT_EQ(effective_pixel_count(cc, pclk, 60, buf), 512);
    EXPECT_EQ(cc.pixel_count, 1024);  // untouched
    const uint16_t at120 = effective_pixel_count(cc, pclk, 120, buf);
    EXPECT_TRUE(at120 > 200 && at120 < 250);                    // ≈235 px: the wire is the limit
    EXPECT_EQ(effective_pixel_count(cc, pclk, 20, buf), 1024);  // buffer cap
    cc.pixel_count = 100;
    EXPECT_EQ(effective_pixel_count(cc, pclk, 120, buf), 100);  // under budget: as set
    cc.pixel_count = 600;
    cc.protocol    = pixfrog::led::Protocol::Off;
    EXPECT_EQ(effective_pixel_count(cc, pclk, 60, buf), 600);
}

// Dead pixels take their share of the physical budget: at 60 Hz a WS2815 line
// carries 512 physical pixels, so 3 dead ones leave 509 live.
static void test_gaps_budget_and_physical_count() {
    pixfrog::config::ChannelConfig cc{};
    cc.protocol         = pixfrog::led::Protocol::WS2815;
    cc.pixel_count      = 1024;
    cc.gaps[0]          = { 0, 1 };
    cc.gaps[1]          = { 200, 2 };
    const size_t buf    = pixfrog::led::kMaxSamplesPerFrame;
    const uint32_t pclk = pixfrog::led::kPclkHz;
    EXPECT_EQ(max_live_pixels_for(cc, pclk, 60, buf), 509);
    EXPECT_EQ(effective_pixel_count(cc, pclk, 60, buf), 509);
    cc.pixel_count = 300;
    EXPECT_EQ(channel_physical_pixels(cc), 303);
    cc.pixel_count = 150;  // line ends before the mid gap
    EXPECT_EQ(channel_physical_pixels(cc), 151);
    // Gaps mean nothing on a disabled channel.
    cc.protocol = pixfrog::led::Protocol::Off;
    EXPECT_EQ(channel_gap_count(cc), 0u);
}

// The ruler is written in physical order: dead pixels in their own colour,
// live ones numbered as live (the 10th live LED is the decade mark).
static void test_preview_with_gaps() {
    const pixfrog::led::PixelGap gaps[1] = { { 0, 1 } };
    uint8_t buf[12 * 3];
    std::memset(buf, 0xEE, sizeof(buf));
    fill_preview_pattern(buf, sizeof(buf), 10, 10, 3, gaps, 1);
    EXPECT_EQ(buf[0], kPreviewDead.r);            // physical 0 = dead
    EXPECT_EQ(buf[1 * 3 + 1], kPreviewGreen.g);   // live 1
    EXPECT_EQ(buf[10 * 3 + 0], kPreviewWhite.r);  // live 10 = physical 10
    EXPECT_EQ(buf[11 * 3], 0xEE);                 // nothing past 11 physical
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
    const SceneV3 base = with_color(mk_scene(kSceneFxSolid, 10, 0, 0, 0, 0), 0, 0, 200);
    uint8_t buf[2 * 3];
    for (uint32_t t : { 0u, 7u, 999u, 123456u }) {
        fill_scene_pattern(buf, sizeof(buf), 2, 3, base, t);
        EXPECT_EQ(buf[0], 10);  // speed 0: never flashes
        EXPECT_EQ(buf[2], 0);
    }
    SceneV3 full = base;
    full.speed   = 255;
    for (uint32_t t : { 0u, 7u, 999u, 123456u }) {
        fill_scene_pattern(buf, sizeof(buf), 2, 3, full, t);
        EXPECT_EQ(buf[0], 0);  // 60 Hz of 1/60 s flashes: always colour 2
        EXPECT_EQ(buf[2], 200);
    }
    // 51 → 12 Hz: period 83.3 ms, flash for the first 16.7 ms of each.
    SceneV3 mid = base;
    mid.speed   = 51;
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 5);
    EXPECT_EQ(buf[2], 200);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 40);
    EXPECT_EQ(buf[0], 10);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, mid, 90);  // next period's flash
    EXPECT_EQ(buf[2], 200);
    // One colour: the flash is black (classic strobe on a lit strip).
    SceneV3 single = mk_scene(kSceneFxSolid, 50, 50, 50, 255, 0);
    fill_scene_pattern(buf, sizeof(buf), 2, 3, single, 3);
    EXPECT_EQ(buf[0], 0);
}

static void test_scene_chase_one_head_per_colour() {
    using namespace pixfrog::config;
    const SceneV3 s     = with_color(mk_scene(kSceneFxChase, 255, 0, 0, 0, 1), 0, 0, 255);
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
    const SceneV3 s  = with_color(mk_scene(kSceneFxBlobs, 0, 0, 255, 60, 3), 255, 0, 0);
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
    SceneV3 s        = with_color(mk_scene(kSceneFxFire, 255, 0, 0, 60, 0), 255, 255, 0);
    constexpr int kN = 60;
    uint8_t buf[kN * 3];
    fill_scene_pattern(buf, sizeof(buf), kN, 3, s, 4321);
    EXPECT_TRUE(buf[0] > 100);            // base glows
    EXPECT_EQ(buf[(kN - 1) * 3 + 1], 0);  // tip burns out
}

static void test_scene_scanner_bounces() {
    using namespace pixfrog::config;
    const SceneV3 s = mk_scene(kSceneFxScanner, 0, 255, 0, 100, 1);
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
    const SceneV3 s = with_color(mk_scene(kSceneFxStripes, 255, 0, 0, 0, 2), 0, 255, 0);
    uint8_t buf[8 * 3];
    fill_scene_pattern(buf, sizeof(buf), 8, 3, s, 0);
    EXPECT_EQ(buf[0 * 3 + 0], 255);
    EXPECT_EQ(buf[1 * 3 + 0], 255);
    EXPECT_EQ(buf[2 * 3 + 1], 255);
    EXPECT_EQ(buf[4 * 3 + 0], 255);
}

static void test_scene_fade_crosses_palette() {
    using namespace pixfrog::config;
    const SceneV3 s = with_color(mk_scene(kSceneFxFade, 255, 0, 0, 255, 0), 0, 0, 255);
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
// A v3 scene and the effect it migrates to draw the same frame, on a plain
// run and through every fixture mode: an even v3 speed halves exactly, and
// Solid's strobe speed is carried over as is.
static void test_migrated_effect_matches_v3_scene() {
    using namespace pixfrog::config;
    constexpr int kN = 60;
    uint8_t a[kN * 4], b[kN * 4];
    ChannelConfig cc{};
    cc.protocol    = pixfrog::led::Protocol::SK6812;
    cc.pixel_count = kN;
    cc.grouping    = 1;
    cc.fixtures[0] = { 0, 20 };
    cc.fixtures[1] = { 25, 10 | kFixtureReversed };
    cc.fixtures[2] = { 40, 20 };
    bool same      = true;
    for (uint8_t fx = 0; fx < kSceneFxCount; ++fx)
        for (uint8_t speed : { 0, 36, 254, 255 }) {
            if (fx != kSceneFxSolid && (speed & 1)) continue;  // odd speeds round up
            for (uint64_t t : { 0ull, 1234ull, 987654321ull }) {
                SceneV3 s      = with_color(mk_scene(fx, 200, 30, 10, speed, 3), 0, 90, 255);
                const Effect e = effect_from_scene_v3(s);
                std::memset(a, 0xAA, sizeof(a));
                std::memset(b, 0xAA, sizeof(b));
                fill_scene_pattern(a, sizeof(a), kN, 4, s, t);
                fill_effect_run(b, sizeof(b), kN, 4, e, t);
                same = same && std::memcmp(a, b, sizeof(a)) == 0;
                for (uint8_t mode = 0; mode < kFixtureModeCount; ++mode) {
                    s.fixture_mode = mode;
                    fill_scene_on_channel(a, sizeof(a), cc, 4, s, t);
                    fill_effect_on_channel(b, sizeof(b), cc, 4, e, mode, t);
                    same = same && std::memcmp(a, b, sizeof(a)) == 0;
                }
            }
        }
    EXPECT_TRUE(same);

    // An unknown generator falls back to Solid; a too-small buffer is left alone.
    Effect e{};
    e.generator    = 99;
    e.colors[0][0] = 9;
    std::memset(a, 0, sizeof(a));
    fill_effect_run(a, sizeof(a), 2, 3, e, 0);
    EXPECT_EQ(a[0], 9);
    EXPECT_EQ(a[3], 9);
    a[0] = 0x55;
    fill_effect_run(a, 2, 2, 3, e, 0);
    EXPECT_EQ(a[0], 0x55);
}

// One unit of Effect::speed is two of a generator's: the top speed doubles.
static void test_effect_speed_counts_double() {
    using namespace pixfrog::config;
    Effect e{};
    e.generator    = kSceneFxChase;
    e.speed        = 255;
    e.num_colors   = 1;
    e.colors[0][0] = 255;
    EXPECT_EQ(effect_rate(e), 510);
    uint8_t buf[100 * 3];
    fill_effect_run(buf, sizeof(buf), 100, 3, e, 100);  // 510 px/s for 0.1 s
    EXPECT_EQ(buf[51 * 3], 255);
    EXPECT_EQ(buf[50 * 3], 0);
    e.generator = kSceneFxSolid;  // a strobe frequency: its own scale
    EXPECT_EQ(effect_rate(e), 255);
    EXPECT_EQ(effect_speed_from_v3(kSceneFxChase, 255), 128);
    EXPECT_EQ(effect_speed_from_v3(kSceneFxChase, 1), 1);
    EXPECT_EQ(effect_speed_from_v3(kSceneFxChase, 0), 0);
    EXPECT_EQ(effect_speed_from_v3(kSceneFxSolid, 255), 255);
}

// ── Dimmer phaser ───────────────────────────────────────────────────────────

static void test_phaser_waveforms() {
    using namespace pixfrog::config;
    // The sine table: mid at 0, crest a quarter in, trough at three quarters.
    EXPECT_EQ(kSin8[0], 128);
    EXPECT_EQ(kSin8[64], 255);
    EXPECT_EQ(kSin8[128], 128);
    EXPECT_EQ(kSin8[192], 0);
    bool mirror = true;
    for (int i = 1; i < 128; ++i)
        mirror = mirror && kSin8[i] + kSin8[256 - i] == 255;
    EXPECT_TRUE(mirror);

    constexpr uint32_t q = 0x4000;  // a quarter cycle
    EXPECT_EQ(phaser_level(kPhaserSin, 0, 0), 128);
    EXPECT_EQ(phaser_level(kPhaserSin, q, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserSin, 3 * q, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserCos, 0, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserCos, 2 * q, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserRampUp, 0, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserRampUp, 2 * q, 0), 128);
    EXPECT_EQ(phaser_level(kPhaserRampUp, 0xFFFF, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserRampDown, 0, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserRampDown, 0xFFFF, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserTriangle, 0, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserTriangle, q, 0), 128);
    EXPECT_EQ(phaser_level(kPhaserTriangle, 2 * q, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserTriangle, 3 * q, 0), 127);
    EXPECT_EQ(phaser_level(kPhaserBump, 0, 0), 1);
    EXPECT_EQ(phaser_level(kPhaserBump, 2 * q, 0), 255);
    EXPECT_TRUE(phaser_level(kPhaserBump, 0xFFFF, 0) < 8);
    EXPECT_EQ(phaser_level(kPhaserPwm, 0, 0), 255);  // half lit by default
    EXPECT_EQ(phaser_level(kPhaserPwm, 2 * q - 1, 0), 255);
    EXPECT_EQ(phaser_level(kPhaserPwm, 2 * q + 512, 0), 0);
    EXPECT_EQ(phaser_level(kPhaserPwm, q - 512, 64), 255);  // width = the lit share
    EXPECT_EQ(phaser_level(kPhaserPwm, q + 512, 64), 0);
    EXPECT_EQ(phaser_level(kPhaserNone, q, 0), 255);  // no wave: full
    EXPECT_EQ(phaser_level(99, q, 0), 255);
    // The cycle wraps: only the low 16 bits of the phase count.
    EXPECT_EQ(phaser_level(kPhaserRampUp, 0x30000 + 2 * q, 0), 128);

    // Width: the wave runs in that share of the cycle (128/255, about half
    // here), then holds its end.
    EXPECT_TRUE(phaser_level(kPhaserTriangle, q / 2, 128) >= 126);  // half-way up already
    EXPECT_TRUE(phaser_level(kPhaserTriangle, q, 128) >= 253);      // the crest, twice as early
    EXPECT_TRUE(phaser_level(kPhaserTriangle, 2 * q, 128) <= 2);    // done
    EXPECT_EQ(phaser_level(kPhaserTriangle, 3 * q, 128), 0);        // held at the end
    EXPECT_EQ(phaser_level(kPhaserRampUp, 3 * q, 128), 255);        // a ramp holds its top
    EXPECT_EQ(phaser_level(kPhaserRampUp, q, 255), 64);             // 255 = the whole cycle
}

static void test_phaser_dimmer_rate_spread_and_floor() {
    using namespace pixfrog::config;
    Effect e{};
    e.ph_wave = kPhaserRampUp;
    e.ph_rate = 20;  // 20 × 1/20 Hz = one cycle a second
    EXPECT_EQ(phaser_dimmer(e, 0, 10, 0), 0);
    EXPECT_EQ(phaser_dimmer(e, 0, 10, 500), 128);
    EXPECT_EQ(phaser_dimmer(e, 0, 10, 1500), 128);  // periodic
    EXPECT_EQ(phaser_dimmer(e, 7, 10, 500), 128);   // no spread: every pixel in phase
    e.ph_rate = 0;                                  // a still wave
    EXPECT_EQ(phaser_dimmer(e, 0, 10, 123456), 0);

    // Spread 16 = one whole cycle along the run; the wave travels up the run,
    // so a pixel further on is earlier in the cycle.
    e.ph_spread = 16;
    e.ph_rate   = 20;
    EXPECT_EQ(phaser_dimmer(e, 0, 8, 500), 128);
    EXPECT_EQ(phaser_dimmer(e, 2, 8, 500), 64);   // a quarter cycle behind
    EXPECT_EQ(phaser_dimmer(e, 4, 8, 500), 0);    // half a cycle behind
    EXPECT_EQ(phaser_dimmer(e, 2, 8, 750), 128);  // what pixel 0 showed 250 ms ago
    e.flags = kEffectPhaserReverse;
    EXPECT_EQ(phaser_dimmer(e, 2, 8, 500), 192);  // ahead instead
    e.flags = 0;
    EXPECT_EQ(phaser_dimmer(e, 0, 0, 500), 128);  // an empty run is not a division by zero

    // The floor lifts the whole wave: low..255 instead of 0..255.
    e.ph_spread = 0;
    e.ph_low    = 55;
    EXPECT_EQ(phaser_dimmer(e, 0, 8, 0), 55);
    EXPECT_EQ(phaser_dimmer(e, 0, 8, 500), 55 + 200 * 128 / 255);
    e.ph_low = 255;
    EXPECT_EQ(phaser_dimmer(e, 0, 8, 0), 255);
    // A long uptime does not make the wave jump: same phase one cycle later.
    e.ph_low               = 0;
    const uint64_t far_off = 5'000'000'000'000ull;
    EXPECT_EQ(phaser_dimmer(e, 0, 8, far_off + 250), phaser_dimmer(e, 0, 8, far_off + 1250));
}

static config::Effect solid_effect(uint8_t r, uint8_t g, uint8_t b) {
    config::Effect e{};
    e.generator    = config::kSceneFxSolid;
    e.num_colors   = 1;
    e.colors[0][0] = r;
    e.colors[0][1] = g;
    e.colors[0][2] = b;
    return e;
}

static void test_effect_dimmer_layers() {
    using namespace pixfrog::config;
    uint8_t a[8 * 4], b[8 * 4];

    // No phaser, no invert: the generator's frame, untouched.
    Effect e = solid_effect(200, 100, 50);
    fill_generator(a, sizeof(a), 8, 3, e.generator, effect_palette(e), 0, 0, 0);
    fill_effect_run(b, sizeof(b), 8, 3, e, 0);
    EXPECT_EQ(std::memcmp(a, b, 24), 0);

    // A still ramp spread over the run dims each pixel by its own level and
    // keeps the hue. Pixel 0 is at the top of the ramp.
    e.ph_wave   = kPhaserRampDown;
    e.ph_spread = 16;
    fill_effect_run(b, sizeof(b), 8, 3, e, 0);
    EXPECT_EQ(b[0], 200);                       // level 255
    EXPECT_EQ(b[4 * 3], 200 * (128 + 1) >> 8);  // half a cycle behind: level 128
    EXPECT_EQ(b[4 * 3 + 1], 100 * (128 + 1) >> 8);
    EXPECT_TRUE(b[1 * 3] < 40);  // an eighth behind wraps to the ramp's end
    // RGBW: the white byte the generators leave at 0 stays there.
    std::memset(b, 0xEE, sizeof(b));
    fill_effect_run(b, sizeof(b), 8, 4, e, 0);
    EXPECT_EQ(b[0], 200);
    EXPECT_EQ(b[3], 0);

    // An out-of-range wave is no phaser at all.
    e.ph_wave = 77;
    fill_effect_run(b, sizeof(b), 8, 3, e, 0);
    EXPECT_EQ(std::memcmp(a, b, 24), 0);
}

static void test_effect_dimmer_invert() {
    using namespace pixfrog::config;
    uint8_t b[16 * 3];

    // A steady colour inverted is dark: every pixel is as bright as colour 1.
    Effect e = solid_effect(128, 0, 0);
    e.flags  = kEffectDimmerInvert;
    fill_effect_run(b, sizeof(b), 4, 3, e, 0);
    EXPECT_EQ(b[0] + b[1] + b[2], 0);

    // With a phaser it is the complement, measured against colour 1 — never
    // brighter than the colour itself.
    e.ph_wave   = kPhaserRampDown;
    e.ph_spread = 16;
    fill_effect_run(b, sizeof(b), 8, 3, e, 0);
    EXPECT_EQ(b[0], 0);                              // phaser full → dark
    EXPECT_EQ(b[4 * 3], 128 - (128 * 129 >> 8));     // phaser half → the other half
    EXPECT_TRUE(b[1 * 3] > 100 && b[1 * 3] <= 128);  // phaser near 0 → nearly full
    EXPECT_EQ(b[1 * 3 + 1], 0);                      // the hue is kept

    // A chase inverted: a lit strip with a dark gap running through it.
    Effect c{};
    c.generator    = kSceneFxChase;
    c.num_colors   = 1;
    c.colors[0][1] = 90;
    c.flags        = kEffectDimmerInvert;
    fill_effect_run(b, sizeof(b), 16, 3, c, 0);  // the head is on pixel 0
    EXPECT_EQ(b[1], 0);
    EXPECT_EQ(b[5 * 3 + 1], 90);
    EXPECT_EQ(b[5 * 3], 0);

    // A pixel brighter than colour 1 (another palette colour) just goes dark.
    Effect two       = c;
    two.num_colors   = 2;
    two.colors[1][0] = 255;  // the second head, at half the run
    fill_effect_run(b, sizeof(b), 16, 3, two, 0);
    EXPECT_EQ(b[8 * 3] + b[8 * 3 + 1] + b[8 * 3 + 2], 0);

    // Rainbow has its own hue everywhere: the invert follows the phaser and a
    // pixel the phaser blacked out comes back in its own colour.
    Effect r{};
    r.generator = kSceneFxRainbow;
    r.flags     = kEffectDimmerInvert;
    r.ph_wave   = kPhaserPwm;  // half the run lit, half dark
    r.ph_spread = 16;
    uint8_t plain[16 * 3];
    Effect rp  = r;
    rp.flags   = 0;
    rp.ph_wave = kPhaserNone;
    fill_effect_run(plain, sizeof(plain), 16, 3, rp, 0);
    fill_effect_run(b, sizeof(b), 16, 3, r, 0);
    EXPECT_EQ(b[0] + b[1] + b[2], 0);  // lit by the phaser → dark
    bool kept = false, dark_side_lit = true;
    for (int i = 1; i < 8; ++i) {  // the phaser's dark half
        const uint8_t* p = b + i * 3;
        dark_side_lit    = dark_side_lit && p[0] + p[1] + p[2] > 0;
        kept             = kept || std::memcmp(p, plain + i * 3, 3) == 0;
    }
    EXPECT_TRUE(dark_side_lit);
    EXPECT_TRUE(kept);
}

static void test_scene_all_effects_bounded() {
    using namespace pixfrog::config;
    constexpr int kN = 1024;
    static uint8_t buf[kN * 4 + 8];
    for (uint8_t fx = 0; fx < kSceneFxCount; ++fx)
        for (int n : { 1, 2, 7, kN })
            for (uint8_t speed : { 0, 1, 255 })
                for (uint8_t param : { 0, 1, 255 }) {
                    SceneV3 s = with_color(
                        with_color(mk_scene(fx, 255, 1, 2, speed, param), 3, 4, 5), 6, 7, 8);
                    s = with_color(s, 9, 10, 11);
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

// ── Show control ─────────────────────────────────────────────────────────────

static void test_show_master_scaling() {
    uint8_t b[4] = { 255, 128, 1, 0 };
    apply_master(b, 4, kMasterFull);  // unchanged
    EXPECT_EQ(b[0], 255);
    EXPECT_EQ(b[1], 128);
    apply_master(b, 4, 32768);  // ~half, rounded
    EXPECT_EQ(b[0], 128);
    EXPECT_EQ(b[1], 64);
    EXPECT_EQ(b[3], 0);
    apply_master(b, 4, 0);
    EXPECT_EQ(b[0] | b[1] | b[2] | b[3], 0);
}

static void test_show_crossfade_weight_and_blend() {
    EXPECT_EQ(fade_weight(0, 1000), 0);
    EXPECT_EQ(fade_weight(1000, 1000), 256);
    EXPECT_EQ(fade_weight(5, 0), 256);  // no fade = the new source at once
    EXPECT_EQ(fade_weight(500, 1000), 128);
    EXPECT_TRUE(fade_weight(100, 1000) < 26);  // eased: slow start
    uint32_t prev = 0;
    for (uint32_t t = 0; t <= 1000; t += 50) {  // monotonic
        EXPECT_TRUE(fade_weight(t, 1000) >= prev);
        prev = fade_weight(t, 1000);
    }
    uint8_t to[2] = { 200, 0 }, from[2] = { 0, 200 };
    blend_into(to, from, 2, 128);
    EXPECT_EQ(to[0], 100);
    EXPECT_EQ(to[1], 100);
    uint8_t keep[1] = { 42 }, other[1] = { 0 };
    blend_into(keep, other, 1, 256);
    EXPECT_EQ(keep[0], 42);
}

static void test_show_strobe() {
    EXPECT_EQ(strobe_hz10_from_dmx(0), 0);
    EXPECT_EQ(strobe_hz10_from_dmx(1), 10);     // 1 Hz
    EXPECT_EQ(strobe_hz10_from_dmx(255), 250);  // 25 Hz
    EXPECT_TRUE(strobe_lit(12345, 0));          // off = steady
    // 10 Hz: 100 ms period, 30 ms flash.
    EXPECT_TRUE(strobe_lit(1000, 100));
    EXPECT_TRUE(strobe_lit(1029, 100));
    EXPECT_TRUE(!(strobe_lit(1030, 100)));
    EXPECT_TRUE(!(strobe_lit(1099, 100)));
    EXPECT_TRUE(strobe_lit(1100, 100));
    // 25 Hz: 40 ms period, flash capped at half of it.
    EXPECT_TRUE(strobe_lit(19, 250));
    EXPECT_TRUE(!(strobe_lit(20, 250)));
}

static void test_show_bands_and_effects() {
    EXPECT_EQ(dmx_band(0), 0);
    EXPECT_EQ(dmx_band(7), 0);
    EXPECT_EQ(dmx_band(8), 1);
    EXPECT_EQ(dmx_band(15), 1);
    EXPECT_EQ(dmx_band(255), 31);
    EXPECT_EQ(effect_from_dmx(0), -1);
    EXPECT_EQ(effect_from_dmx(1), 0);
    EXPECT_EQ(effect_from_dmx(255), config::kSceneFxCount - 1);
    // Every effect is reachable, in order.
    int prev = 0, seen = 1;
    for (int v = 2; v <= 255; ++v) {
        const int e = effect_from_dmx(static_cast<uint8_t>(v));
        EXPECT_TRUE(e == prev || e == prev + 1);
        if (e != prev) ++seen;
        prev = e;
    }
    EXPECT_EQ(seen, config::kSceneFxCount);
}

static void test_control_presets_and_footprint() {
    config::ControlConfig c = config::default_control();
    EXPECT_EQ(c.enabled, 0);
    EXPECT_EQ(c.count, 5);  // master(16) blackout strobe scene fade
    EXPECT_EQ(config::control_footprint(c), 6);
    config::control_apply_preset(c, config::ControlPreset::Full);
    EXPECT_EQ(c.count, 15);
    EXPECT_EQ(config::control_footprint(c), 16);
    EXPECT_TRUE(c.slots[c.count - 1].fn == static_cast<uint8_t>(config::CtlFn::Fseq));
}

static void test_control_sanitize() {
    config::ControlConfig c = config::default_control();
    c.count                 = 40;
    c.address               = 0;
    c.universe              = 0x9000;
    c.slots[1].fn           = 99;                    // unknown
    c.slots[2].flags        = config::kCtlFlagFine;  // fine on a Blackout
    c.slots[3].mask         = 0;
    config::sanitize_control(c);
    EXPECT_EQ(c.count, config::kMaxControlSlots);
    EXPECT_EQ(c.address, 1);
    EXPECT_EQ(c.universe, config::kDefaultControlUniverse);
    EXPECT_TRUE(c.slots[1].fn == static_cast<uint8_t>(config::CtlFn::None));
    EXPECT_EQ(c.slots[2].flags, 0);
    EXPECT_EQ(c.slots[3].mask, 0xFF);
    // A mode that would run past slot 512 is trimmed to what fits.
    config::ControlConfig d = config::default_control();  // 6 channels
    d.address               = 509;
    config::sanitize_control(d);
    EXPECT_EQ(config::control_footprint(d), 4);  // 509..512
    EXPECT_EQ(d.count, 3);
}

static void test_control_evaluate() {
    config::ControlConfig c{};
    c.enabled  = 1;
    c.address  = 10;
    c.slots[0] = config::control_slot(config::CtlFn::Master, 0xFF, 0, config::kCtlFlagFine);
    c.slots[1] = config::control_slot(config::CtlFn::Master, 0x0F);  // group dimmer, multiplies
    c.slots[2] = config::control_slot(config::CtlFn::Blackout, 0xF0);
    c.slots[3] = config::control_slot(config::CtlFn::Strobe);
    c.slots[4] = config::control_slot(config::CtlFn::Scene, 0x03);
    c.slots[5] = config::control_slot(config::CtlFn::Red, 0xFF, 1);
    c.slots[6] = config::control_slot(config::CtlFn::Blue, 0xFF, 1);
    c.slots[7] = config::control_slot(config::CtlFn::Fade);
    c.slots[8] = config::control_slot(config::CtlFn::Speed);
    c.count    = 9;
    uint8_t u[512]{};
    u[9]  = 0x80;  // master coarse
    u[10] = 0x00;  // master fine → 0x8000
    u[11] = 255;   // group master full
    u[12] = 200;   // blackout outputs 5-8
    u[13] = 1;     // strobe 1 Hz
    u[14] = 17;    // scene band 2
    u[15] = 255;   // colour 2 red
    u[16] = 0;     // colour 2 blue
    u[17] = 30;    // 3 s fade
    u[18] = 0;     // speed: the scene's own
    ControlEval ev;
    evaluate_control(c, u, sizeof(u), ev);
    EXPECT_EQ(ev.master[0], 0x8000);
    EXPECT_EQ(ev.master[7], 0x8000);
    EXPECT_EQ(ev.blackout, 0xF0);
    EXPECT_EQ(ev.strobe_hz10[3], 10);
    EXPECT_EQ(ev.n_scene, 1);
    EXPECT_EQ(ev.scene_band[0], 2);
    EXPECT_EQ(ev.scene_mask[0], 0x03);
    EXPECT_EQ(ev.fade_ms, 3000);
    EXPECT_EQ(ev.ovr[0].speed, -1);
    EXPECT_EQ(ev.ovr[0].color[1][0], 255);
    EXPECT_EQ(ev.ovr[0].color[1][1], 0);   // unpatched component reads 0
    EXPECT_EQ(ev.ovr[0].color[0][0], -1);  // colour 1 untouched
    EXPECT_EQ(ev.fseq_band, -1);           // no FSEQ slot
    // Group master: outputs 1-4 at half × half.
    u[11] = 128;
    evaluate_control(c, u, sizeof(u), ev);
    EXPECT_TRUE(ev.master[0] > 0x3F00 && ev.master[0] < 0x4100);
    EXPECT_EQ(ev.master[4], 0x8000);
    // Colour at 0,0,0 = the scene's own; a short packet reads as zeros.
    u[15] = 0;
    evaluate_control(c, u, 12, ev);
    EXPECT_EQ(ev.ovr[0].color[1][0], -1);
    EXPECT_EQ(ev.blackout, 0);
    EXPECT_EQ(ev.fade_ms, 0);
}

static void test_effect_override_applies() {
    config::Effect e{};
    e.generator  = config::kSceneFxSolid;
    e.speed      = 10;
    e.num_colors = 1;
    EffectOverride o;
    o.speed       = 200;
    o.generator   = config::kSceneFxChase;
    o.color[2][0] = 9;
    o.color[2][1] = 8;
    o.color[2][2] = 7;
    apply_effect_override(e, o);
    EXPECT_EQ(e.speed, 200);
    EXPECT_EQ(e.generator, config::kSceneFxChase);
    EXPECT_EQ(config::effect_num_colors(e), 3);  // colour 3 exists now
    EXPECT_EQ(e.colors[2][0], 9);
    EXPECT_EQ(e.colors[2][2], 7);
    EffectOverride none;
    config::Effect before = e;
    apply_effect_override(e, none);
    EXPECT_EQ(std::memcmp(&before, &e, sizeof(e)), 0);
}

// Largest per-byte change between two frames.
static int max_step(const uint8_t* a, const uint8_t* b, size_t n) {
    int m = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > m) m = d;
    }
    return m;
}

// The scene clock is 64-bit: crossing 2^32 ms (49.7 days) is just the next
// millisecond for every effect but fire (documented reseed) — no jump.
static void test_scene_clock_has_no_wrap_jump() {
    using namespace pixfrog::config;
    const uint8_t fxs[]   = { kSceneFxSolid,    kSceneFxChase,  kSceneFxRainbow, kSceneFxBlobs,
                              kSceneFxGradient, kSceneFxFade,   kSceneFxTwinkle, kSceneFxScanner,
                              kSceneFxWave,     kSceneFxStripes };
    constexpr uint16_t kN = 60;
    const uint64_t wrap   = 1ull << 32;
    for (uint8_t fx : fxs) {
        SceneV3 s = with_color(mk_scene(fx, 255, 0, 0, 200, 0), 0, 0, 255);
        uint8_t a[kN * 3], b[kN * 3], c[kN * 3], d[kN * 3];
        // Steady state: one ms apart, far from any wrap.
        fill_scene_pattern(a, sizeof(a), kN, 3, s, 1'000'000);
        fill_scene_pattern(b, sizeof(b), kN, 3, s, 1'000'001);
        // Across 2^32 ms.
        fill_scene_pattern(c, sizeof(c), kN, 3, s, wrap - 1);
        fill_scene_pattern(d, sizeof(d), kN, 3, s, wrap);
        const int steady = max_step(a, b, sizeof(a));
        const int across = max_step(c, d, sizeof(c));
        if (across > steady + 16)
            std::printf("effect %u jumps at 2^32 ms: %d vs %d\n", fx, across, steady);
        EXPECT_TRUE(across <= steady + 16);
    }
}

int main() {
    test_scene_clock_has_no_wrap_jump();
    test_total_bytes_rgb();
    test_total_bytes_rgbw();
    test_universes_used();
    test_universes_off();
    test_auto_patch_cascade();
    test_t_dma_ws2815();
    test_emission_budget();
    test_capacity_check();
    test_max_pixels_ws2815();
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
    test_effective_pixel_count_non_destructive();
    test_gaps_budget_and_physical_count();
    test_preview_with_gaps();
    test_layout_whole_pixels_never_split_one();
    test_layout_counts_the_start_address();
    test_layout_one_fixture_per_universe();
    test_decode_per_fixture_leaves_the_rest_dark();
    test_auto_patch_compact_and_forced_packing();
    test_universe_map_shares_a_universe();
    test_fixture_spans_skip_the_dead_leds();
    test_fixture_spans_follow_invert_and_grouping();
    test_fixture_modes_each_chain_mirror();
    test_fixtures_normalize_drops_overlaps();
    test_reversed_fixture_runs_backwards();
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
    test_migrated_effect_matches_v3_scene();
    test_effect_speed_counts_double();
    test_phaser_waveforms();
    test_phaser_dimmer_rate_spread_and_floor();
    test_effect_dimmer_layers();
    test_effect_dimmer_invert();
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
    test_show_master_scaling();
    test_show_crossfade_weight_and_blend();
    test_show_strobe();
    test_show_bands_and_effects();
    test_control_presets_and_footprint();
    test_control_sanitize();
    test_control_evaluate();
    test_effect_override_applies();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
