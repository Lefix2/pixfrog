// parlio_output.cpp against a recording PARLIO TX driver: the frame handed to
// the hardware is decoded back (NRZ bit timing on the channel's bus bit), so
// these cases prove pixel bytes → wire, the triple-buffer rotation and pacing,
// unit lifecycle on frame-length changes, calibration patterns and every
// driver failure.

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

// NRZ decode of bus bit `bit` from a frame: one bit per high pulse, '1' when
// the pulse is longer than half the bit period (20 samples for WS281x).
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

void channel0(led::Protocol p, uint16_t px, led::ColorOrder order = led::ColorOrder::GRB) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch == 0 ? p : led::Protocol::Off;
        c.pixel_count    = px;
        c.universe_start = 1;
        c.dmx_start      = 1;
        c.color_order    = order;
        c.brightness     = 255;
        c.gamma_x10      = 10;
        c.wb_r = c.wb_g = c.wb_b = 255;
        std::memset(c.gaps, 0, sizeof(c.gaps));
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    dmx::handle_pending_remaps();
}

// Live data for channel 0, decoded into its front pixel buffer.
void pixels(const Bytes& rgb) {
    dmx::write_universe_from_source(1, rgb.data(), rgb.size(), 0x0A000001,
                                    dmx::kArtnetMergeTimeoutUs);
    dmx::note_channel_activity(0);
    dmx::swap_universes();
    dmx::decode_pixels_for_channel(0);
    dmx::swap_pixels(0);
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
}

}  // namespace

TEST(init_validates_its_config_and_resources) {
    output::InitConfig bad = cfg();
    bad.bus_gpio_16        = nullptr;
    EXPECT_FALSE(output::init(bad));
    EXPECT_FALSE(output::init(cfg(0)));
    shim::psram_present(false);
    EXPECT_FALSE(output::init(cfg()));
    shim::psram_present(true);
    shim::fail_next(shim::Fault::HeapCaps, 1, 1);  // the second frame buffer
    EXPECT_FALSE(output::init(cfg()));
}

TEST(all_channels_off_creates_no_unit_and_renders_nothing) {
    channel0(led::Protocol::Off, 4);
    shim::parlio_reset();
    EXPECT_TRUE(output::init(cfg()));
    EXPECT_EQ(shim::parlio_log().units_created, 0);
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_EQ(shim::parlio_log().transmits, 0);
    EXPECT_EQ(output::fb_bytes(), 0u);
}

TEST(pixels_reach_the_wire_in_colour_order) {
    channel0(led::Protocol::WS2815, 2);
    shim::parlio_reset();
    EXPECT_TRUE(output::init(cfg()));
    pixels({ 0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC });
    EXPECT_TRUE(output::render_frame(100));
    const auto& log = shim::parlio_log();
    EXPECT_EQ(log.transmits, 1);
    EXPECT_TRUE(log.loop);
    EXPECT_EQ(log.clk_hz, led::kPclkHz);
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(log.data_gpios[i], kBus[i]);           // bus bit i → its GPIO
    const Bytes wire = decode_nrz(log.last_samples, 0);  // channel 0 = bus bit 0
    EXPECT_EQ(wire.size(), 6u);
    if (wire.size() == 6) {
        EXPECT_EQ(wire[0], 0x22);  // GRB
        EXPECT_EQ(wire[1], 0x11);
        EXPECT_EQ(wire[2], 0x33);
        EXPECT_EQ(wire[3], 0xBB);
    }
    EXPECT_EQ(output::get_debug_counters().trans_done, 1u);
    EXPECT_TRUE(output::fb_bytes() > 0);
}

TEST(three_buffers_rotate_and_a_reused_one_waits_a_frame) {
    channel0(led::Protocol::WS2815, 200);
    shim::parlio_reset();
    EXPECT_TRUE(output::init(cfg()));
    std::vector<const void*> bufs;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(output::render_frame(100));
        bufs.push_back(shim::parlio_log().last_buffer);
    }
    EXPECT_TRUE(bufs[0] != bufs[1] && bufs[1] != bufs[2] && bufs[0] != bufs[2]);
    EXPECT_TRUE(bufs[3] == bufs[0]);  // back to the first one
    // Rendering back to back: the 4th had to wait for the 1st to drain.
    const int64_t t0 = shim::now_us();
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_TRUE(shim::now_us() > t0);
    output::wait_idle();
    output::dump_stats();
}

TEST(a_new_frame_length_recreates_the_unit_and_off_destroys_it) {
    channel0(led::Protocol::WS2815, 4);
    shim::parlio_reset();
    EXPECT_TRUE(output::init(cfg()));
    EXPECT_TRUE(output::render_frame(100));
    const int created = shim::parlio_log().units_created;
    channel0(led::Protocol::WS2815, 300);
    EXPECT_TRUE(output::render_frame(100));
    EXPECT_EQ(shim::parlio_log().units_created, created + 1);
    EXPECT_TRUE(shim::parlio_log().units_deleted >= 1);
    EXPECT_TRUE(output::render_frame(100));  // same length: same unit
    EXPECT_EQ(shim::parlio_log().units_created, created + 1);
    channel0(led::Protocol::Off, 4);
    EXPECT_TRUE(output::render_frame(100));  // the bus goes quiet
    EXPECT_EQ(output::fb_bytes(), 0u);
}

TEST(a_frame_past_the_buffer_capacity_is_refused) {
    channel0(led::Protocol::Off, 4);
    EXPECT_TRUE(output::init(cfg(20000)));  // far below what 1024 px need
    channel0(led::Protocol::WS2815, 1024);
    EXPECT_FALSE(output::render_frame(100));
    EXPECT_FALSE(output::init(cfg(20000)));  // init sizes the unit up front: refused too
}

TEST(calibration_patterns_and_the_gpio_probe) {
    channel0(led::Protocol::WS2815, 4);
    shim::parlio_reset();
    EXPECT_TRUE(output::init(cfg()));
    output::set_calibration_mode(1);
    EXPECT_EQ(output::get_calibration_mode(), 1);
    for (uint8_t p = 0; p < 3; ++p)
        EXPECT_TRUE(output::emit_calibration_pattern(p));
    EXPECT_EQ(shim::parlio_log().transmits, 3);
    const int gpio0 = shim::gpio_calls();
    EXPECT_TRUE(output::emit_calibration_pattern(3));  // bit-bang probe
    EXPECT_TRUE(output::emit_calibration_pattern(3));
    EXPECT_TRUE(shim::gpio_calls() > gpio0);
    output::set_calibration_mode(-1);
    channel0(led::Protocol::Off, 4);
    EXPECT_TRUE(output::init(cfg(1000)));  // too small for the calibration frame
    EXPECT_FALSE(output::emit_calibration_pattern(0));
}

TEST(driver_failures_are_reported) {
    channel0(led::Protocol::WS2815, 4);
    EXPECT_TRUE(output::init(cfg()));
    channel0(led::Protocol::WS2815, 5);  // force a new unit
    shim::fail_next(shim::Fault::ParlioNew);
    EXPECT_FALSE(output::render_frame(100));
    shim::fail_next(shim::Fault::ParlioEnable);
    EXPECT_FALSE(output::render_frame(100));
    shim::fail_next(shim::Fault::ParlioTransmit);
    EXPECT_FALSE(output::render_frame(100));
    EXPECT_TRUE(output::render_frame(100));  // recovers
    channel0(led::Protocol::WS2815, 6);
    shim::fail_next(shim::Fault::ParlioNew);
    EXPECT_FALSE(output::init(cfg()));  // init creates the unit up front
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
