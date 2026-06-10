// Host-side unit tests for led_protocols.
// Verify that timings derived from PCLK = 16 MHz match docs/PROTOCOLS.md §3.3
// and that encode_channel correctly OR-s into the destination buffer.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "led_protocols.h"

using namespace pixfrog::led;

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

// ── Timings ─────────────────────────────────────────────────────────────────

static void test_timing_ws2815() {
    const auto t = timing_for(Protocol::WS2815);
    EXPECT_EQ(t.samples_t0h, 5);
    EXPECT_EQ(t.samples_t1h, 15);
    EXPECT_EQ(t.samples_bit, 20);
    EXPECT_EQ(t.samples_reset, 4480);
}

static void test_timing_ws2812b() {
    const auto t = timing_for(Protocol::WS2812B);
    EXPECT_EQ(t.samples_t0h, 6);
    EXPECT_EQ(t.samples_t1h, 11);
    EXPECT_EQ(t.samples_bit, 20);
}

static void test_timing_apa102_clock() {
    // 4 MHz CLOCK → samples_per_clock = 4 at PCLK=16MHz.
    auto t = timing_for(Protocol::APA102, 4'000'000);
    EXPECT_EQ(t.samples_per_clock, 4);

    // 8 MHz CLOCK → samples_per_clock = 2.
    t = timing_for(Protocol::APA102, 8'000'000);
    EXPECT_EQ(t.samples_per_clock, 2);

    // 1 MHz CLOCK → samples_per_clock = 16.
    t = timing_for(Protocol::APA102, 1'000'000);
    EXPECT_EQ(t.samples_per_clock, 16);
}

// ── Bytes per pixel ─────────────────────────────────────────────────────────

static void test_bytes_per_pixel() {
    EXPECT_EQ(bytes_per_pixel(Protocol::WS2815), 3);
    EXPECT_EQ(bytes_per_pixel(Protocol::WS2812B), 3);
    EXPECT_EQ(bytes_per_pixel(Protocol::SK6812), 4);
    EXPECT_EQ(bytes_per_pixel(Protocol::WS2814), 4);
    EXPECT_EQ(bytes_per_pixel(Protocol::DMX512), 1);  // one byte per DMX slot
    EXPECT_EQ(bytes_per_pixel(Protocol::Off), 0);     // disabled → no bytes
}

// ── Off (disabled channel) ──────────────────────────────────────────────────

static void test_off_produces_nothing() {
    EXPECT_TRUE(is_off(Protocol::Off));
    EXPECT_TRUE(!is_off(Protocol::WS2815));

    ChannelDesc d{};
    d.protocol    = Protocol::Off;
    d.pixel_count = 144;  // stale pixel_count must be ignored when Off
    EXPECT_EQ(encoded_size_samples(d), 0);

    uint16_t out[8]     = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    const uint8_t px[1] = { 0xAB };
    EXPECT_EQ(encode_channel(d, px, out, 8), 0);  // writes nothing
    EXPECT_EQ(out[0], 0xFFFF);                    // buffer untouched
}

// ── DMX512 output encoder ───────────────────────────────────────────────────

static void test_dmx_size() {
    ChannelDesc d{};
    d.protocol     = Protocol::DMX512;
    d.pixel_count  = 512;
    d.bus_bit_data = 0;

    // BREAK(1536) + MAB(192) + (512 slots + 1 start code) × 11 bits × 64 samples.
    const size_t expected = 1536 + 192 + (512 + 1) * 11 * 64;
    EXPECT_EQ(encoded_size_samples(d), expected);
}

static void test_dmx_waveform() {
    ChannelDesc d{};
    d.protocol      = Protocol::DMX512;
    d.pixel_count   = 1;  // single slot + null start code
    d.bus_bit_data  = 0;  // DATA+ = bit 0
    d.bus_bit_clock = 1;  // DATA− (complement) = bit 1

    const uint8_t slots[1] = { 0x01 };  // LSB set → only data bit 0 is high
    const size_t cap       = encoded_size_samples(d);
    std::vector<uint16_t> out(cap, 0);
    const size_t written = encode_channel(d, slots, out.data(), cap);
    EXPECT_EQ(written, cap);

    constexpr int kBit   = 64;
    constexpr int kBreak = 1536;
    constexpr int kMab   = 192;

    // The complement line (bit 1) must always be the inverse of DATA (bit 0).
    for (size_t i = 0; i < cap; ++i)
        EXPECT_EQ((out[i] & 1) ^ ((out[i] >> 1) & 1), 1);

    // BREAK is entirely LOW.
    for (int i = 0; i < kBreak; ++i)
        EXPECT_EQ(out[i] & 1, 0);
    // MAB is entirely HIGH.
    for (int i = kBreak; i < kBreak + kMab; ++i)
        EXPECT_EQ(out[i] & 1, 1);

    // Start code 0x00: start bit LOW, then 8 data bits LOW, then 2 stop HIGH.
    const int sc = kBreak + kMab;  // first sample of the start-code character
    for (int i = 0; i < 9 * kBit; ++i)
        EXPECT_EQ(out[sc + i] & 1, 0);  // start + 8 zero data bits
    for (int i = 9 * kBit; i < 11 * kBit; ++i)
        EXPECT_EQ(out[sc + i] & 1, 1);  // 2 stop bits

    // Slot value 0x01: start LOW, data bit0 HIGH (LSB first), bits1..7 LOW.
    const int sl = sc + 11 * kBit;
    for (int i = 0; i < kBit; ++i)
        EXPECT_EQ(out[sl + i] & 1, 0);  // start bit
    for (int i = kBit; i < 2 * kBit; ++i)
        EXPECT_EQ(out[sl + i] & 1, 1);  // data bit 0 = 1
    for (int i = 2 * kBit; i < 9 * kBit; ++i)
        EXPECT_EQ(out[sl + i] & 1, 0);  // data bits 1..7 = 0
}

static void test_dmx_or_into_buffer() {
    ChannelDesc d{};
    d.protocol      = Protocol::DMX512;
    d.pixel_count   = 4;
    d.bus_bit_data  = 6;  // some other channel's DATA bit
    d.bus_bit_clock = 7;  // its complement bit

    const uint8_t slots[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    const size_t cap       = encoded_size_samples(d);
    std::vector<uint16_t> out(cap, 0);

    const uint16_t foreign_mask = (1u << 0) | (1u << 3) | (1u << 14);
    for (auto& s : out)
        s = foreign_mask;

    encode_channel(d, slots, out.data(), cap);

    // OR-only semantics: foreign bits must be preserved, and the DATA/complement
    // pair must always be mutually exclusive.
    const uint16_t data_mask = static_cast<uint16_t>(1u << 6);
    const uint16_t comp_mask = static_cast<uint16_t>(1u << 7);
    for (size_t i = 0; i < cap; ++i) {
        EXPECT_TRUE((out[i] & foreign_mask) == foreign_mask);
        const bool data = out[i] & data_mask;
        const bool comp = out[i] & comp_mask;
        EXPECT_TRUE(data != comp);  // exactly one of the pair is asserted
    }
}

// ── NRZ encoder: one pixel, verify exact bit layout ────────────────────────

static void test_nrz_one_pixel_ws2815() {
    ChannelDesc d{};
    d.protocol         = Protocol::WS2815;
    d.color_order      = ColorOrder::RGB;
    d.pixel_count      = 1;
    d.brightness       = 255;
    d.grouping         = 1;
    d.invert_direction = false;
    d.bus_bit_data     = 0;
    d.bus_bit_clock    = 1;

    const uint8_t pixels[3] = { 0x80, 0x00, 0x00 };  // R=128, G=B=0
    const size_t cap        = encoded_size_samples(d);
    std::vector<uint16_t> out(cap, 0);
    const size_t written = encode_channel(d, pixels, out.data(), cap);

    EXPECT_EQ(written, cap);

    // R=0x80 = 0b10000000 → first bit '1', next 7 bits '0'
    // Each bit takes 20 samples (samples_bit). Bit '1' → 15 high, 5 low.
    // We check the first 20 samples (bit 7 of R).
    int high_count = 0;
    for (int i = 0; i < 20; ++i)
        high_count += (out[i] & 1) ? 1 : 0;
    EXPECT_EQ(high_count, 15);  // T1H = 15 samples

    // Bits 6..0 of R are '0' → 5 high, 15 low each.
    high_count = 0;
    for (int i = 20; i < 40; ++i)
        high_count += (out[i] & 1) ? 1 : 0;
    EXPECT_EQ(high_count, 5);  // T0H = 5 samples

    // CLOCK bit (bit 1) must stay 0 on all samples for 1-wire.
    for (size_t i = 0; i < cap; ++i)
        EXPECT_EQ(out[i] & 2, 0);
}

// ── NRZ encoder: OR-into-buffer must preserve other channels' bits ─────────

static void test_nrz_or_into_buffer() {
    ChannelDesc d{};
    d.protocol      = Protocol::WS2815;
    d.color_order   = ColorOrder::RGB;
    d.pixel_count   = 1;
    d.brightness    = 255;
    d.grouping      = 1;
    d.bus_bit_data  = 4;  // some other channel
    d.bus_bit_clock = 5;

    const uint8_t pixels[3] = { 0xFF, 0xFF, 0xFF };
    const size_t cap        = encoded_size_samples(d);
    std::vector<uint16_t> out(cap, 0);

    // Pre-fill some bits owned by "another channel" (bits 0, 2, 14).
    const uint16_t foreign_mask = (1u << 0) | (1u << 2) | (1u << 14);
    for (auto& s : out)
        s = foreign_mask;

    encode_channel(d, pixels, out.data(), cap);

    // All samples must still have the foreign bits set (OR-only semantics).
    for (size_t i = 0; i < cap; ++i) {
        EXPECT_TRUE((out[i] & foreign_mask) == foreign_mask);
    }
}

// ── SPI encoder size ────────────────────────────────────────────────────────

static void test_spi_size() {
    ChannelDesc d{};
    d.protocol      = Protocol::APA102;
    d.color_order   = ColorOrder::BGR;
    d.pixel_count   = 16;
    d.brightness    = 255;
    d.grouping      = 1;
    d.bus_bit_data  = 2;
    d.bus_bit_clock = 3;
    d.clock_hz      = 4'000'000;

    const size_t sz = encoded_size_samples(d);
    // start(4) + 16 pixels × 4 bytes + end(2) = 70 bytes → 560 bits → ×4 samples = 2240
    EXPECT_EQ(sz, 70 * 8 * 4);
}

// ── Perf: WS2815 1024 px must encode ≤ 20 ms on a reference CI runner ──────
//
// 20 ms is the budget for a single frame at ~50 Hz target on the ESP32-P4
// (the actual hardware runs at 400 MHz; a typical CI runner at 2-4 GHz
// will be 5-10× faster, so this is a generous ceiling that still catches
// regressions where the encoder accidentally becomes O(n^2) or similar).

static void test_perf_encode_ws2815_1024() {
    ChannelDesc d{};
    d.protocol         = Protocol::WS2815;
    d.color_order      = ColorOrder::GRB;
    d.pixel_count      = 1024;
    d.brightness       = 255;
    d.grouping         = 1;
    d.invert_direction = false;
    d.bus_bit_data     = 0;
    d.bus_bit_clock    = 1;

    std::vector<uint8_t> pixels(1024 * 3, 0xAA);
    const size_t cap = encoded_size_samples(d);
    std::vector<uint16_t> samples(cap, 0);

    constexpr int kReps = 10;
    auto t0             = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kReps; ++i) {
        std::fill(samples.begin(), samples.end(), uint16_t{ 0 });
        encode_channel(d, pixels.data(), samples.data(), cap);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    const double per_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;
    std::printf("perf: encode WS2815 1024 px (incl. zero-fill) = %.3f ms/call\n", per_ms);

    if (per_ms < 20.0)
        g_pass++;
    else {
        g_fail++;
        std::fprintf(stderr, "FAIL perf: %.3f ms >= 20 ms\n", per_ms);
    }
}

// ── encode_frame: single-pass output must equal per-channel OR composition ──

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

// Reference = zeroed buffer + encode_channel per channel (the documented OR
// semantics). encode_frame gets a 0xFFFF-poisoned buffer to prove it does not
// rely on pre-zeroing.
static void check_frame_equivalence(const std::vector<ChannelDesc>& descs,
                                    const std::vector<std::vector<uint8_t>>& px, int src_line) {
    size_t frame_len = 0;
    for (const auto& d : descs) {
        const size_t len = encoded_size_samples(d);
        if (len > frame_len) frame_len = len;
    }
    const size_t cap = frame_len + 97;  // slack to catch out-of-range writes

    std::vector<uint16_t> ref(cap, 0);
    std::vector<const uint8_t*> ptrs;
    for (size_t i = 0; i < descs.size(); ++i) {
        ptrs.push_back(px[i].data());
        if (!is_off(descs[i].protocol)) encode_channel(descs[i], px[i].data(), ref.data(), cap);
    }

    std::vector<uint16_t> got(cap, 0xFFFF);
    const size_t written = encode_frame(descs.data(), ptrs.data(), descs.size(), got.data(), cap);
    EXPECT_EQ(written, frame_len);

    size_t mismatches = 0;
    for (size_t i = 0; i < frame_len; ++i) {
        if (got[i] != ref[i]) {
            if (mismatches < 4) {
                std::fprintf(stderr, "  (case line %d) sample %zu: got 0x%04X want 0x%04X\n",
                             src_line, i, got[i], ref[i]);
            }
            mismatches++;
        }
    }
    EXPECT_EQ(mismatches, 0);
    for (size_t i = frame_len; i < cap; ++i) {
        if (got[i] != 0xFFFF) mismatches++;  // must not write past frame_len
    }
    EXPECT_EQ(mismatches, 0);
}

static ChannelDesc make_desc(size_t ch, Protocol p, uint16_t pixels) {
    ChannelDesc d{};
    d.protocol      = p;
    d.color_order   = is_rgbw(p) ? ColorOrder::GRBW : ColorOrder::GRB;
    d.pixel_count   = pixels;
    d.brightness    = 255;
    d.grouping      = 1;
    d.bus_bit_data  = static_cast<uint8_t>(ch * 2);
    d.bus_bit_clock = static_cast<uint8_t>(ch * 2 + 1);
    d.clock_hz      = 4'000'000;
    return d;
}

static std::vector<uint8_t> random_pixels(const ChannelDesc& d) {
    std::vector<uint8_t> px(static_cast<size_t>(d.pixel_count) * bytes_per_pixel(d.protocol) + 1);
    for (auto& b : px)
        b = static_cast<uint8_t>(rnd());
    return px;
}

static void test_frame_single_channel() {
    std::vector<ChannelDesc> descs{ make_desc(0, Protocol::WS2815, 37) };
    std::vector<std::vector<uint8_t>> px{ random_pixels(descs[0]) };
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_homogeneous_8ch() {
    std::vector<ChannelDesc> descs;
    std::vector<std::vector<uint8_t>> px;
    for (size_t ch = 0; ch < 8; ++ch) {
        descs.push_back(make_desc(ch, Protocol::WS2812B, 512));
        px.push_back(random_pixels(descs.back()));
    }
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_mixed_t0h_and_lengths() {
    // Same samples_bit (20), different T0H/T1H, ragged lengths → dropout path.
    std::vector<ChannelDesc> descs{
        make_desc(0, Protocol::WS2815, 512), make_desc(1, Protocol::WS2812B, 300),
        make_desc(2, Protocol::WS2811, 1),   make_desc(3, Protocol::WS2814, 64),
        make_desc(4, Protocol::WS2815, 7),
    };
    descs[0].invert_direction = true;
    descs[1].brightness       = 128;
    descs[3].grouping         = 3;
    std::vector<std::vector<uint8_t>> px;
    for (const auto& d : descs)
        px.push_back(random_pixels(d));
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_two_groups_sk6812() {
    // SK6812 has samples_bit=19 → second sweep group, OR-ed over the first.
    std::vector<ChannelDesc> descs{
        make_desc(0, Protocol::SK6812, 200),
        make_desc(1, Protocol::WS2815, 150),
        make_desc(2, Protocol::SK6812, 33),
        make_desc(3, Protocol::WS2812B, 400),
    };
    std::vector<std::vector<uint8_t>> px;
    for (const auto& d : descs)
        px.push_back(random_pixels(d));
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_full_mix() {
    // NRZ + clocked + DMX + Off + zero-length on one bus.
    std::vector<ChannelDesc> descs{
        make_desc(0, Protocol::WS2815, 256), make_desc(1, Protocol::APA102, 100),
        make_desc(2, Protocol::DMX512, 512), make_desc(3, Protocol::Off, 99),
        make_desc(4, Protocol::SK6812, 80),  make_desc(5, Protocol::LPD8806, 50),
        make_desc(6, Protocol::WS2812B, 0),  make_desc(7, Protocol::WS2811, 1024),
    };
    std::vector<std::vector<uint8_t>> px;
    for (const auto& d : descs)
        px.push_back(random_pixels(d));
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_fuzz() {
    const Protocol protos[] = { Protocol::Off,    Protocol::WS2815,  Protocol::WS2812B,
                                Protocol::WS2811, Protocol::SK6812,  Protocol::WS2814,
                                Protocol::APA102, Protocol::LPD8806, Protocol::DMX512 };
    for (int iter = 0; iter < 25; ++iter) {
        std::vector<ChannelDesc> descs;
        std::vector<std::vector<uint8_t>> px;
        for (size_t ch = 0; ch < 8; ++ch) {
            ChannelDesc d = make_desc(ch, protos[rnd() % 9], static_cast<uint16_t>(rnd() % 600));
            d.brightness  = static_cast<uint8_t>(rnd());
            d.grouping    = static_cast<uint8_t>(1 + rnd() % 8);
            d.invert_direction  = (rnd() & 1) != 0;
            const auto co_count = static_cast<uint8_t>(ColorOrder::COUNT);
            d.color_order       = static_cast<ColorOrder>(rnd() % co_count);
            descs.push_back(d);
            px.push_back(random_pixels(d));
        }
        check_frame_equivalence(descs, px, __LINE__);
    }
}

static void test_frame_all_dmx() {
    // 8 DMX channels, ragged slot counts incl. 0 (BREAK+MAB+start code only).
    std::vector<ChannelDesc> descs;
    std::vector<std::vector<uint8_t>> px;
    const uint16_t slot_counts[8] = { 512, 1, 0, 256, 512, 100, 511, 7 };
    for (size_t ch = 0; ch < 8; ++ch) {
        descs.push_back(make_desc(ch, Protocol::DMX512, slot_counts[ch]));
        px.push_back(random_pixels(descs.back()));
    }
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_dmx_dominant() {
    // DMX region (363 k samples) larger than the NRZ region → DMX must win
    // the pure-store pass and the NRZ groups OR over it.
    std::vector<ChannelDesc> descs{
        make_desc(0, Protocol::DMX512, 512),
        make_desc(1, Protocol::WS2815, 256),
        make_desc(2, Protocol::SK6812, 100),
        make_desc(3, Protocol::APA102, 60),
    };
    std::vector<std::vector<uint8_t>> px;
    for (const auto& d : descs)
        px.push_back(random_pixels(d));
    check_frame_equivalence(descs, px, __LINE__);
}

static void test_frame_capacity_too_small() {
    std::vector<ChannelDesc> descs{ make_desc(0, Protocol::WS2815, 10) };
    std::vector<std::vector<uint8_t>> px{ random_pixels(descs[0]) };
    const uint8_t* ptrs[1] = { px[0].data() };
    uint16_t out[16];
    EXPECT_EQ(encode_frame(descs.data(), ptrs, 1, out, 16), 0);
}

// ── Perf: 8 × WS2815 512 px single-pass must beat the 60 FPS CPU budget ─────
//
// Same generous-ceiling logic as test_perf_encode_ws2815_1024: a CI runner is
// 5-10× an ESP32-P4 core, so 8 ms here ≈ comfortably under 16.7 ms on target.

static void test_perf_frame_8x512() {
    std::vector<ChannelDesc> descs;
    std::vector<std::vector<uint8_t>> px;
    std::vector<const uint8_t*> ptrs;
    for (size_t ch = 0; ch < 8; ++ch) {
        descs.push_back(make_desc(ch, Protocol::WS2815, 512));
        px.push_back(random_pixels(descs.back()));
    }
    for (const auto& p : px)
        ptrs.push_back(p.data());

    size_t cap = 0;
    for (const auto& d : descs)
        cap = std::max(cap, encoded_size_samples(d));
    std::vector<uint16_t> samples(cap);

    constexpr int kReps = 20;
    auto t0             = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kReps; ++i)
        encode_frame(descs.data(), ptrs.data(), descs.size(), samples.data(), cap);
    auto t1 = std::chrono::high_resolution_clock::now();

    const double per_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;
    std::printf("perf: encode_frame 8×WS2815 512 px = %.3f ms/call\n", per_ms);

    if (per_ms < 8.0)
        g_pass++;
    else {
        g_fail++;
        std::fprintf(stderr, "FAIL perf: encode_frame %.3f ms >= 8 ms\n", per_ms);
    }
}

// 8 full DMX universes: the largest frame region (363 k samples). The sweep
// must keep this a single store pass, not 8 RMW traversals.
static void test_perf_frame_8xdmx512() {
    std::vector<ChannelDesc> descs;
    std::vector<std::vector<uint8_t>> px;
    std::vector<const uint8_t*> ptrs;
    for (size_t ch = 0; ch < 8; ++ch) {
        descs.push_back(make_desc(ch, Protocol::DMX512, 512));
        px.push_back(random_pixels(descs.back()));
    }
    for (const auto& p : px)
        ptrs.push_back(p.data());

    size_t cap = 0;
    for (const auto& d : descs)
        cap = std::max(cap, encoded_size_samples(d));
    std::vector<uint16_t> samples(cap);

    constexpr int kReps = 20;
    auto t0             = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kReps; ++i)
        encode_frame(descs.data(), ptrs.data(), descs.size(), samples.data(), cap);
    auto t1 = std::chrono::high_resolution_clock::now();

    const double per_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;
    std::printf("perf: encode_frame 8×DMX512 = %.3f ms/call\n", per_ms);

    if (per_ms < 8.0)
        g_pass++;
    else {
        g_fail++;
        std::fprintf(stderr, "FAIL perf: encode_frame 8×DMX512 %.3f ms >= 8 ms\n", per_ms);
    }
}

int main() {
    test_timing_ws2815();
    test_timing_ws2812b();
    test_timing_apa102_clock();
    test_bytes_per_pixel();
    test_off_produces_nothing();
    test_dmx_size();
    test_dmx_waveform();
    test_dmx_or_into_buffer();
    test_nrz_one_pixel_ws2815();
    test_nrz_or_into_buffer();
    test_spi_size();
    test_perf_encode_ws2815_1024();
    test_frame_single_channel();
    test_frame_homogeneous_8ch();
    test_frame_mixed_t0h_and_lengths();
    test_frame_two_groups_sk6812();
    test_frame_full_mix();
    test_frame_fuzz();
    test_frame_all_dmx();
    test_frame_dmx_dominant();
    test_frame_capacity_too_small();
    test_perf_frame_8x512();
    test_perf_frame_8xdmx512();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
