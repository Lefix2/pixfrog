// lcd_cam_output.cpp (the legacy LED backend) against a recording RGB panel:
// the frame clocked out at each refresh is decoded back (NRZ bit timing on the
// channel's bus bit), so these cases prove pixel bytes → wire, the line
// geometry the bit periods impose, the double-buffer swap and vsync pacing,
// tail zeroing when a frame shrinks, calibration patterns and every driver
// failure.

#include <cstring>
#include <vector>

#include "config_store.h"
#include "dmx_manager.h"
#include "harness.h"
#include "led_output.h"
#include "led_protocols.h"
#include "shim_control.h"

using namespace pixfrog;
using Bytes = std::vector<uint8_t>;

namespace {

const int kBus[16] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 };

output::InitConfig cfg(uint32_t max_samples = led::kMaxSamplesPerFrame) {
    output::InitConfig c{};
    c.bus_gpio_16           = kBus;
    c.pclk_hz               = led::kPclkHz;
    c.max_samples_per_frame = max_samples;
    return c;
}

// NRZ decode of bus bit `bit`: one bit per high pulse, '1' when the pulse is
// longer than half the WS281x bit period (20 samples).
Bytes decode_nrz(const std::vector<uint16_t>& s, int bit) {
    Bytes out;
    uint8_t cur = 0;
    int nbits   = 0;
    size_t i    = 0;
    while (i < s.size()) {
        if (!((s[i] >> bit) & 1)) {
            ++i;
            continue;
        }
        size_t hi = 0;
        while (i < s.size() && ((s[i] >> bit) & 1)) {
            ++hi;
            ++i;
        }
        cur = static_cast<uint8_t>((cur << 1) | (hi > 10 ? 1 : 0));
        if (++nbits == 8) {
            out.push_back(cur);
            cur   = 0;
            nbits = 0;
        }
    }
    return out;
}

// Channel 0 = p0 with px pixels, channel 1 = p1 (Off by default), rest Off.
void channels(led::Protocol p0, uint16_t px, led::Protocol p1 = led::Protocol::Off,
              uint32_t clock_hz = 0) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch == 0 ? p0 : ch == 1 ? p1 : led::Protocol::Off;
        c.pixel_count    = px;
        c.universe_start = static_cast<uint16_t>(1 + ch);
        c.dmx_start      = 1;
        c.color_order    = led::ColorOrder::GRB;
        c.brightness     = 255;
        c.gamma_x10      = 10;
        c.clock_hz       = clock_hz;
        c.wb_r = c.wb_g = c.wb_b = 255;
        std::memset(c.gaps, 0, sizeof(c.gaps));
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    dmx::handle_pending_remaps();
}

void pixels(const Bytes& rgb) {
    dmx::write_universe_from_source(1, rgb.data(), rgb.size(), 0x0A000001,
                                    dmx::kArtnetMergeTimeoutUs);
    dmx::note_channel_activity(0);
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(0);
    dmx::swap_pixels(0);
}

// Highest sample index (exclusive) where any bus bit is high.
size_t last_high(const std::vector<uint16_t>& s) {
    size_t n = s.size();
    while (n && !s[n - 1])
        --n;
    return n;
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        dmx::init();
        once = true;
    }
    shim::faults_clear();
    shim::psram_present(true);
    shim::psram_largest_block(0);
    shim::lcd_auto_vsync(true);
}

}  // namespace

TEST(init_validates_its_config_and_resources) {
    channels(led::Protocol::Off, 4);
    output::InitConfig bad = cfg();
    bad.bus_gpio_16        = nullptr;
    EXPECT_FALSE(output::init(bad));
    EXPECT_FALSE(output::init(cfg(0)));
    shim::psram_present(false);
    EXPECT_FALSE(output::init(cfg()));
    shim::psram_present(true);
    shim::fail_next(shim::Fault::Semaphore);
    EXPECT_FALSE(output::init(cfg()));
    shim::lcd_reset();
    EXPECT_TRUE(output::init(cfg()));  // all off: the panel is lazy
    EXPECT_EQ(shim::lcd_log().panels_created, 0);
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_EQ(shim::lcd_log().refreshes, 0);
    EXPECT_EQ(output::fb_bytes(), 0u);
}

TEST(pixels_reach_the_wire_in_colour_order) {
    channels(led::Protocol::WS2815, 2);
    shim::lcd_reset();
    EXPECT_TRUE(output::init(cfg()));  // something to emit: panel created up front
    const auto& log = shim::lcd_log();
    EXPECT_EQ(log.panels_created, 1);
    EXPECT_EQ(log.pclk_hz, led::kPclkHz);
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(log.data_gpios[i], kBus[i]);
    pixels({ 0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC });
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_EQ(log.draws, 1);
    EXPECT_EQ(log.refreshes, 1);  // draw_bitmap alone emits nothing
    const Bytes wire = decode_nrz(log.last_frame, 0);
    EXPECT_EQ(wire.size(), 6u);
    if (wire.size() == 6) {
        EXPECT_EQ(wire[0], 0x22);  // GRB
        EXPECT_EQ(wire[1], 0x11);
        EXPECT_EQ(wire[2], 0x33);
        EXPECT_EQ(wire[3], 0xBB);
        EXPECT_EQ(wire[4], 0xAA);
        EXPECT_EQ(wire[5], 0xCC);
    }
    const auto dc = output::get_debug_counters();
    EXPECT_EQ(dc.trans_done, 1u);
    EXPECT_EQ(dc.vsync, 1u);
    EXPECT_EQ(dc.msync_err, 0u);
    EXPECT_TRUE(output::fb_bytes() > 0);
}

TEST(a_line_is_a_multiple_of_every_nrz_bit_period) {
    channels(led::Protocol::WS2815, 50);  // 20-sample bits
    EXPECT_TRUE(output::init(cfg()));
    shim::lcd_reset();
    channels(led::Protocol::WS2815, 51);
    EXPECT_TRUE(output::render_frame(100));
    const uint32_t h20 = shim::lcd_log().h_res;
    EXPECT_EQ(h20 % 20, 0u);
    EXPECT_TRUE(h20 > 4000 && h20 <= 4093);  // as long as the 12-bit timing allows
    channels(led::Protocol::WS2815, 50, led::Protocol::SK6812);  // + 19-sample bits
    EXPECT_TRUE(output::render_frame(100));
    const uint32_t h = shim::lcd_log().h_res;
    EXPECT_EQ(h % 20, 0u);
    EXPECT_EQ(h % 19, 0u);  // the inter-line blank never lands mid-bit
    EXPECT_EQ(h, 3800u);
    EXPECT_EQ(shim::lcd_log().panels_created, 2);  // a new geometry, a new panel
    EXPECT_EQ(shim::lcd_log().panels_deleted, 2);
    // Clocked channels do not constrain the line.
    channels(led::Protocol::APA102, 50, led::Protocol::WS2815);
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_EQ(shim::lcd_log().h_res, h20);
    // Enough lines for the frame, and no more.
    const uint32_t v = shim::lcd_log().v_res;
    EXPECT_TRUE(static_cast<size_t>(v) * h20 >= output::fb_bytes() / 2);
    EXPECT_EQ(output::fb_bytes(), static_cast<size_t>(h20) * v * 2);
}

TEST(two_buffers_alternate_and_each_waits_for_vsync) {
    channels(led::Protocol::WS2815, 100);
    shim::lcd_reset();
    EXPECT_TRUE(output::init(cfg()));
    std::vector<const void*> drawn;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(output::render_frame(100));
        drawn.push_back(shim::lcd_log().last_drawn);
    }
    EXPECT_TRUE(drawn[0] != drawn[1]);
    EXPECT_TRUE(drawn[2] == drawn[0]);
    // An emission whose VSYNC_END never comes: the next frame times out
    // (underrun), then self-heals instead of blocking forever.
    shim::lcd_auto_vsync(false);
    EXPECT_TRUE(output::render_frame(100));  // kicks, but never completes
    const int64_t t0 = shim::now_us();
    EXPECT_FALSE(output::render_frame(100));  // waited 100 ms for the vsync
    EXPECT_TRUE(shim::now_us() - t0 >= 100000);
    shim::lcd_auto_vsync(true);
    EXPECT_TRUE(output::render_frame(100));  // re-armed
    output::wait_idle();
    output::dump_stats();
}

TEST(a_shrinking_frame_zeroes_the_stale_tail) {
    // A WS281x shrink inside the same line count only uncovers its own reset
    // tail (zeros already); a clocked APA102 frame ends right after its data,
    // so 200 → 190 px leaves 10 stale pixels if nobody clears them.
    channels(led::Protocol::APA102, 200);
    EXPECT_TRUE(output::init(cfg()));
    shim::lcd_reset();
    pixels(Bytes(200 * 3, 0xFF));
    EXPECT_TRUE(output::render_frame(100));
    const size_t long_end = last_high(shim::lcd_log().last_frame);
    EXPECT_TRUE(output::render_frame(100));  // both buffers hold 200 lit pixels
    const uint32_t v = shim::lcd_log().v_res;
    channels(led::Protocol::APA102, 190);
    pixels(Bytes(190 * 3, 0xFF));
    EXPECT_TRUE(output::render_frame(100));        // into the buffer that held 200
    EXPECT_EQ(shim::lcd_log().panels_created, 0);  // same geometry, no reallocation
    EXPECT_EQ(shim::lcd_log().v_res, v);
    EXPECT_TRUE(last_high(shim::lcd_log().last_frame) + 10 * 32 * 4 <= long_end);
}

TEST(frames_past_the_capacity_or_the_psram_are_refused) {
    channels(led::Protocol::Off, 4);
    EXPECT_TRUE(output::init(cfg(20000)));
    channels(led::Protocol::WS2815, 1024);
    EXPECT_FALSE(output::render_frame(100));  // over the configured cap
    EXPECT_FALSE(output::init(cfg(20000)));
    // PSRAM too fragmented for one frame buffer.
    channels(led::Protocol::WS2815, 100);
    shim::psram_largest_block(1024);
    EXPECT_FALSE(output::init(cfg()));
}

TEST(calibration_patterns_and_the_gpio_probe) {
    channels(led::Protocol::WS2815, 4);
    shim::lcd_reset();
    EXPECT_TRUE(output::init(cfg()));
    output::set_calibration_mode(1);
    EXPECT_EQ(output::get_calibration_mode(), 1);
    for (uint8_t p = 0; p < 3; ++p)
        EXPECT_TRUE(output::emit_calibration_pattern(p));
    EXPECT_EQ(shim::lcd_log().refreshes, 3);
    EXPECT_TRUE(shim::lcd_log().h_res * shim::lcd_log().v_res >= 262144u);
    const int gpio0 = shim::gpio_calls();
    EXPECT_TRUE(output::emit_calibration_pattern(3));  // bit-bang probe
    EXPECT_TRUE(output::emit_calibration_pattern(3));
    EXPECT_TRUE(shim::gpio_calls() > gpio0);
    // A calibration emission that never completes: the next one times out.
    shim::lcd_auto_vsync(false);
    EXPECT_TRUE(output::emit_calibration_pattern(0));
    EXPECT_FALSE(output::emit_calibration_pattern(0));
    shim::lcd_auto_vsync(true);
    shim::fail_next(shim::Fault::LcdDraw);
    EXPECT_FALSE(output::emit_calibration_pattern(0));
    shim::fail_next(shim::Fault::LcdRefresh);
    EXPECT_FALSE(output::emit_calibration_pattern(0));
    EXPECT_TRUE(output::emit_calibration_pattern(0));  // recovers
    output::set_calibration_mode(-1);
    EXPECT_FALSE(output::init(cfg(1000)) && output::emit_calibration_pattern(0));
}

TEST(driver_failures_are_reported) {
    channels(led::Protocol::WS2815, 4);
    shim::lcd_reset();
    EXPECT_TRUE(output::init(cfg()));
    const shim::Fault create_faults[] = { shim::Fault::LcdNewPanel, shim::Fault::LcdPanelReset,
                                          shim::Fault::LcdPanelInit, shim::Fault::LcdFrameBuffer };
    uint16_t px                       = 5;
    for (shim::Fault f : create_faults) {
        channels(led::Protocol::WS2815, px += 200);  // force a new panel
        shim::fail_next(f);
        EXPECT_FALSE(output::render_frame(100));
    }
    EXPECT_TRUE(output::render_frame(100));
    shim::fail_next(shim::Fault::LcdDraw);
    EXPECT_FALSE(output::render_frame(100));
    shim::fail_next(shim::Fault::LcdRefresh);
    EXPECT_FALSE(output::render_frame(100));
    shim::fail_next(shim::Fault::CacheMsync);
    EXPECT_TRUE(output::render_frame(100));  // logged and counted, not fatal
    EXPECT_EQ(output::get_debug_counters().msync_err, 1u);
    EXPECT_TRUE(output::render_frame(100));  // recovers
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
