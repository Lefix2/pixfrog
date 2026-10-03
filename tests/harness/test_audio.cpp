// audio.cpp + synth.h — the speaker path: the real driver on the I2C + I2S
// shims with a model ES8311, its playback task run on the fake clock, and the
// little sounds themselves.
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "audio.h"
#include "es8311_regs.h"
#include "harness.h"
#include "shim_control.h"
#include "synth.h"

using namespace pixfrog;
namespace synth = pixfrog::audio::synth;

namespace {

struct Es8311 : shim::I2cDevice {
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    uint8_t regs[256]{};
    bool write(const uint8_t* d, size_t n) override {
        if (n != 2) return false;
        writes.emplace_back(d[0], d[1]);
        regs[d[0]] = d[1];
        return true;
    }
    bool read(uint8_t*, size_t) override { return true; }
    bool wrote(uint8_t reg, uint8_t val) const {
        for (const auto& w : writes)
            if (w.first == reg && w.second == val) return true;
        return false;
    }
};

Es8311 g_codec;
uint8_t g_volume = 100;  // the setting the driver reads

i2c_master_bus_handle_t bus() {
    static i2c_master_bus_handle_t b = nullptr;
    if (!b) {
        i2c_master_bus_config_t c{};
        i2c_new_master_bus(&c, &b);
    }
    return b;
}

audio::InitConfig cfg() {
    audio::InitConfig c{};
    c.i2c_bus   = bus();
    c.mclk_gpio = 13;
    c.bclk_gpio = 12;
    c.ws_gpio   = 10;
    c.dout_gpio = 9;
    c.din_gpio  = 11;
    c.volume    = [] { return g_volume; };
    return c;
}

// Plays what is queued: the task renders it, then waits 2 s and mutes, then
// blocks (the second blocking point ends the run).
void drain() {
    shim::run_task_for("audio", 2, nullptr, true);
}

int peak(const std::vector<int16_t>& s, size_t from, size_t to) {
    int p = 0;
    for (size_t i = from; i < to && i < s.size(); ++i)
        p = std::abs(s[i]) > p ? std::abs(s[i]) : p;
    return p;
}

// Sign changes over samples [from, to) of a rendered sound (mono index).
template <class F> int crossings(F f, uint32_t from, uint32_t to) {
    int n      = 0;
    float prev = f(from);
    for (uint32_t k = from + 1; k < to; ++k) {
        const float v = f(k);
        if ((v < 0) != (prev < 0)) ++n;
        prev = v;
    }
    return n;
}

}  // namespace

// Ordered first: init() succeeds once per process.
TEST(failures_leave_the_box_without_sound) {
    shim::i2s_reset();
    audio::InitConfig none = cfg();
    none.i2c_bus           = nullptr;
    EXPECT_FALSE(audio::init(none));
    shim::i2c_attach(0x18, nullptr);  // no codec: it NACKs
    EXPECT_FALSE(audio::init(cfg()));
    EXPECT_EQ(shim::i2s_log().deleted, 1);  // the I2S channel is given back
    shim::i2c_attach(0x18, &g_codec);
    shim::fail_next(shim::Fault::I2sNew);
    EXPECT_FALSE(audio::init(cfg()));
    shim::fail_next(shim::Fault::QueueCreate);
    EXPECT_FALSE(audio::init(cfg()));
    EXPECT_FALSE(audio::ready());
    EXPECT_FALSE(audio::play_tone(440, 100));
    EXPECT_FALSE(audio::start_test());
    audio::feedback(audio::Feedback::Tick);  // no audio: silently nothing
}

TEST(init_starts_i2s_then_brings_the_codec_up_muted) {
    shim::i2s_reset();
    g_codec.writes.clear();
    EXPECT_TRUE(audio::init(cfg()));
    EXPECT_TRUE(audio::ready());
    EXPECT_TRUE(audio::init(cfg()));  // once only
    EXPECT_TRUE(shim::task_created("audio"));
    const auto& l = shim::i2s_log();
    EXPECT_TRUE(l.enabled);
    EXPECT_TRUE(l.auto_clear);  // an underrun is silence, not the last buffers replayed
    EXPECT_EQ(l.sample_rate, audio::kSampleRate);
    EXPECT_EQ(l.mclk_multiple, 256u);  // the codec's clock table: MCLK = 256 fs
    EXPECT_EQ(l.mclk, 13);
    EXPECT_EQ(l.bclk, 12);
    EXPECT_EQ(l.ws, 10);
    EXPECT_EQ(l.dout, 9);  // to the codec's DAC (DSDIN)
    EXPECT_TRUE(!g_codec.writes.empty() && g_codec.writes[0].first == 0x00 &&
                g_codec.writes[0].second == 0x1F);         // a reset first
    EXPECT_EQ(g_codec.regs[0x00], 0x80);                   // powered, I2S slave
    EXPECT_EQ(g_codec.regs[0x01], 0x3F);                   // clocks from the MCLK pin
    EXPECT_EQ(g_codec.regs[0x09], 0x0C);                   // 16-bit I2S into the DAC
    EXPECT_EQ(g_codec.regs[0x12], 0x00);                   // DAC powered
    EXPECT_EQ(g_codec.regs[0x32], 0xBF);                   // 0 dB: the volume is digital
    EXPECT_EQ(g_codec.regs[0x31], audio::es8311::kMuted);  // silent until a sound plays
}

namespace {
constexpr size_t kPreRoll = audio::kSampleRate * 30 / 1000;  // silence after unmuting
}  // namespace

TEST(the_chime_plays_from_the_task_then_the_codec_mutes_after_quiet) {
    shim::i2s_reset();
    shim::i2s_log().enabled = true;
    g_codec.writes.clear();
    EXPECT_TRUE(audio::start_test());  // returns at once: only queued
    EXPECT_TRUE(shim::i2s_log().samples.empty());
    drain();
    const auto& s = shim::i2s_log().samples;
    EXPECT_EQ(s.size(), (kPreRoll + synth::length(synth::Sound::Chime)) * 2);
    EXPECT_EQ(peak(s, 0, kPreRoll * 2), 0);  // the unmute ramp runs on silence
    const int p = peak(s, 0, s.size());
    EXPECT_TRUE(p > 19400 && p < 19900);  // normalized to 0.6 of full scale: no clipping
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        if (s[i] != s[i + 1]) {
            EXPECT_TRUE(false);  // both channels carry the same sound
            break;
        }
    EXPECT_TRUE(g_codec.writes.size() >= 2 && g_codec.writes[0].first == 0x31 &&
                g_codec.writes[0].second == audio::es8311::kUnmuted);  // unmuted first
    EXPECT_EQ(g_codec.regs[0x31], audio::es8311::kMuted);              // muted after 10 s
    EXPECT_EQ(g_codec.regs[0x32], audio::es8311::kVolume0dB);          // the DAC never boosts
}

TEST(a_fast_knob_plays_the_latest_sound_only) {
    shim::i2s_reset();
    shim::i2s_log().enabled = true;
    for (int i = 0; i < 7; ++i)
        audio::feedback(audio::Feedback::Tick);  // a fast spin before the task runs
    audio::feedback(audio::Feedback::Bump);      // replaces the waiting tick
    drain();
    EXPECT_EQ(shim::i2s_log().samples.size(), (kPreRoll + synth::length(synth::Sound::Bump)) * 2);
    EXPECT_TRUE(audio::play_tone(880, 120));
    drain();
}

TEST(a_new_sound_cuts_the_playing_one_short) {
    shim::i2s_reset();
    shim::i2s_log().enabled  = true;
    static size_t cut_at     = 0;
    shim::i2s_log().on_write = [](size_t writes, void*) {
        if (writes == 20) {  // pre-roll done, the chime well under way
            cut_at = shim::i2s_log().samples.size() / 2;
            audio::feedback(audio::Feedback::Tick);
        }
    };
    audio::start_test();
    drain();
    shim::i2s_log().on_write = nullptr;
    const auto& s            = shim::i2s_log().samples;
    // The chime stops after a 32-sample fade, then the tick plays whole.
    EXPECT_EQ(s.size(), (cut_at + 32 + synth::length(synth::Sound::Tick)) * 2);
    EXPECT_TRUE(std::abs(s[(cut_at + 31) * 2]) < 1500);  // faded to almost nothing
}

TEST(a_failed_write_is_survived) {
    shim::i2s_log().enabled = true;
    shim::fail_next(shim::Fault::I2sWrite);
    audio::feedback(audio::Feedback::Bump);
    drain();  // logged, and the task carries on
    EXPECT_EQ(g_codec.regs[0x31], audio::es8311::kMuted);
}

TEST(the_volume_setting_is_followed_and_off_is_silence) {
    shim::i2s_reset();
    shim::i2s_log().enabled = true;
    g_volume                = 0;  // the default: off
    EXPECT_FALSE(audio::play_tone(440, 50));
    EXPECT_FALSE(audio::start_test());
    EXPECT_FALSE(audio::play_boot());
    audio::feedback(audio::Feedback::Tick);
    drain();
    EXPECT_TRUE(shim::i2s_log().samples.empty());  // nothing played
    g_volume = 50;  // -20 dB: a tenth (the tick: 10 dB under the rest)
    audio::feedback(audio::Feedback::Tick);
    drain();
    const int half = peak(shim::i2s_log().samples, 0, shim::i2s_log().samples.size());
    EXPECT_TRUE(half > 580 && half < 680);
    shim::i2s_reset();
    shim::i2s_log().enabled = true;
    g_volume                = 250;  // out of range reads as 100
    audio::feedback(audio::Feedback::Tick);
    drain();
    const int full = peak(shim::i2s_log().samples, 0, shim::i2s_log().samples.size());
    EXPECT_TRUE(full > 6100 && full < 6450);
    g_volume = 100;
}

// On a gauge the tick rises an octave from the low bound to the high one.
TEST(a_gauge_tick_follows_the_value_over_an_octave) {
    auto zero_crossings = [](float level) {
        shim::i2s_reset();
        shim::i2s_log().enabled = true;
        audio::feedback(audio::Feedback::Tick, level);
        drain();
        const auto& s = shim::i2s_log().samples;
        int n         = 0;
        for (size_t i = (kPreRoll + 8) * 2; i + 2 < (kPreRoll + 80) * 2; i += 2)
            if ((s[i] < 0) != (s[i + 2] < 0)) ++n;
        return n;
    };
    const int low = zero_crossings(0.0f), high = zero_crossings(1.0f);
    EXPECT_TRUE(high > low * 17 / 10 && high < low * 23 / 10);  // about twice: an octave
    EXPECT_TRUE(zero_crossings(0.5f) > low && zero_crossings(0.5f) < high);
}

// Normalized: every fixed sound reaches the same peak, so none is quieter or
// louder than another.
TEST(every_sound_plays_at_one_level) {
    for (auto f : { audio::Feedback::Bump, audio::Feedback::Confirm, audio::Feedback::Cancel }) {
        shim::i2s_reset();
        shim::i2s_log().enabled = true;
        audio::feedback(f);
        drain();
        const int p = peak(shim::i2s_log().samples, 0, shim::i2s_log().samples.size());
        EXPECT_TRUE(p > 19400 && p < 19900);
    }
    shim::i2s_reset();
    shim::i2s_log().enabled = true;
    EXPECT_TRUE(audio::play_boot());
    drain();
    EXPECT_EQ(shim::i2s_log().samples.size(), (kPreRoll + synth::length(synth::Sound::Croak)) * 2);
}

// ── The sounds ───────────────────────────────────────────────────────────────

TEST(the_tick_is_a_short_woody_knock) {
    const uint32_t n = synth::length(synth::Sound::Tick);
    EXPECT_EQ(n, 400u);  // 25 ms
    auto f    = [](uint32_t k) { return synth::sample(synth::Sound::Tick, k); };
    float hi  = 0;
    float end = 0;
    for (uint32_t k = 0; k < 48; ++k)
        hi = std::fabs(f(k)) > hi ? std::fabs(f(k)) : hi;
    for (uint32_t k = n - 40; k < n; ++k)
        end = std::fabs(f(k)) > end ? std::fabs(f(k)) : end;
    EXPECT_TRUE(hi > 0.3f);                   // its attack in the first 3 ms
    EXPECT_TRUE(end < 0.01f);                 // gone by the end
    EXPECT_TRUE(crossings(f, 16, 160) > 15);  // a knock: kHz partials, not a thud
}

TEST(the_bump_is_a_low_thud) {
    const uint32_t n = synth::length(synth::Sound::Bump);
    EXPECT_EQ(n, 1920u);  // 120 ms
    auto f = [](uint32_t k) { return synth::sample(synth::Sound::Bump, k); };
    // Low: about 2 × 200 Hz × 50 ms sign changes in its body, far below the tick.
    const int c = crossings(f, 32, 32 + 800);
    EXPECT_TRUE(c > 10 && c < 40);
    float tail = 0;
    for (uint32_t k = n - 80; k < n; ++k)
        tail = std::fabs(f(k)) > tail ? std::fabs(f(k)) : tail;
    EXPECT_TRUE(tail < 0.06f);  // decayed by its end
}

TEST(the_chime_is_three_bell_notes_rising) {
    const uint32_t n1 = synth::kRate * synth::kChime[0].ms / 1000;
    const uint32_t n2 = synth::kRate * synth::kChime[1].ms / 1000;
    auto f            = [](uint32_t k) { return synth::sample(synth::Sound::Chime, k); };
    EXPECT_EQ(synth::length(synth::Sound::Chime), n1 + n2 + synth::kRate * 520 / 1000);
    EXPECT_TRUE(std::fabs(f(n1 - 1)) < 0.05f);  // each note fades into the next
    EXPECT_TRUE(std::fabs(f(0)) < 0.01f);       // and starts from silence
    EXPECT_EQ(synth::sample(synth::Sound::Chime, synth::length(synth::Sound::Chime)), 0.0f);
    EXPECT_EQ(synth::length(synth::Sound::Tone, 100), 1600u);
    EXPECT_EQ(synth::length(synth::Sound::Count), 0u);  // not a sound: nothing
    EXPECT_EQ(synth::sample(synth::Sound::Count, 0), 0.0f);
    EXPECT_EQ(synth::sample(synth::Sound::Confirm, synth::length(synth::Sound::Confirm)), 0.0f);
}

TEST(volume_is_even_in_db) {
    EXPECT_EQ(synth::volume_gain(0), 0.0f);                          // off: silent
    EXPECT_TRUE(std::fabs(synth::volume_gain(1) - 0.01f) < 1e-4f);   // -40 dB
    EXPECT_TRUE(std::fabs(synth::volume_gain(100) - 1.0f) < 1e-4f);  // 0 dB
    EXPECT_TRUE(std::fabs(synth::volume_gain(200) - 1.0f) < 1e-4f);
    audio::es8311::RegWrite small[2];
    EXPECT_EQ(audio::es8311::init_sequence(small, 2), 0u);  // too small: nothing
}

TEST(confirm_goes_up_cancel_goes_down) {
    auto rate = [](synth::Sound snd, uint32_t from, uint32_t to) {
        return crossings([snd](uint32_t k) { return synth::sample(snd, k); }, from, to);
    };
    const uint32_t first = synth::kRate * synth::kConfirm[0].ms / 1000;
    EXPECT_TRUE(rate(synth::Sound::Confirm, first + 16, first + 400) >
                rate(synth::Sound::Confirm, 16, 400) * 1.2);
    EXPECT_TRUE(rate(synth::Sound::Cancel, first + 16, first + 400) <
                rate(synth::Sound::Cancel, 16, 400));
}

TEST(the_croak_is_two_low_buzzy_syllables) {
    const uint32_t n = synth::length(synth::Sound::Croak);
    EXPECT_EQ(n, synth::kRate * 320 / 1000);
    float gap = 0, body = 0;
    for (uint32_t k = synth::kRib; k < synth::kRib + synth::kGap; ++k)
        gap = std::fabs(synth::sample(synth::Sound::Croak, k)) > gap
                ? std::fabs(synth::sample(synth::Sound::Croak, k))
                : gap;
    for (uint32_t k = 400; k < 1200; ++k)
        body = std::fabs(synth::sample(synth::Sound::Croak, k)) > body
                 ? std::fabs(synth::sample(synth::Sound::Croak, k))
                 : body;
    EXPECT_EQ(gap, 0.0f);  // "rib" … "bit"
    EXPECT_TRUE(body > 0.3f);
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv);
}
