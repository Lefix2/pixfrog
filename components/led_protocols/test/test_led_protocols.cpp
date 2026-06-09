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

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
