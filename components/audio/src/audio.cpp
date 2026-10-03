#include "audio.h"

#include <cmath>
#include <cstring>

#include "driver/i2s_std.h"
#include "es8311_regs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "synth.h"

namespace pixfrog::audio {

namespace {

constexpr const char* TAG       = "AUDIO";
constexpr int kI2cTimeoutMs     = 50;
constexpr size_t kChunk         = 64;  // stereo frames per i2s write (4 ms)
constexpr size_t kDmaBuffers    = 4;   // 16 ms queued: a new sound is heard at once
constexpr uint32_t kMuteAfterMs = 10000;
constexpr uint32_t kPreRollMs   = 30;    // silence after unmuting: the codec's mute ramp
constexpr uint32_t kFadeOut     = 32;    // samples (2 ms) to end a cut-off sound cleanly
constexpr float kPeak           = 0.6f;  // every sound's peak at 100 %: the amp clipped above

struct Cmd {
    synth::Sound sound;
    uint16_t freq;
    uint16_t ms;
    uint8_t volume;  // 1..100
};

i2c_master_dev_handle_t g_codec = nullptr;
i2s_chan_handle_t g_tx          = nullptr;
QueueHandle_t g_queue           = nullptr;  // a mailbox: the latest request only
uint8_t (*g_volume)()           = nullptr;
float g_norm[static_cast<size_t>(synth::Sound::Count)];  // per sound: to kPeak

uint8_t volume_now() {
    if (!g_volume) return 100;
    const uint8_t v = g_volume();
    return v > 100 ? 100 : v;
}

// Each fixed sound's gain to a common peak, measured once: the tick, the thud
// and the chime then play at one loudness whatever their own level.
void measure_sounds() {
    for (size_t i = 0; i < static_cast<size_t>(synth::Sound::Count); ++i) {
        const auto snd = static_cast<synth::Sound>(i);
        float peak     = 0.0f;
        if (snd != synth::Sound::Tone)
            for (uint32_t k = 0; k < synth::length(snd); ++k) {
                const float v = std::fabs(synth::sample(snd, k));
                peak          = v > peak ? v : peak;
            }
        // The tick repeats with every knob step, and at 1.5-3 kHz it sits where
        // the little speaker is most efficient: kept 10 dB under the rest.
        const float trim = snd == synth::Sound::Tick ? 0.32f : 1.0f;
        g_norm[i]        = (peak > 0.0f ? kPeak / peak : kPeak / 0.6f) * trim;  // Tone: its bell
    }
}

bool reg(uint8_t r, uint8_t v) {
    const uint8_t buf[2] = { r, v };
    return i2c_master_transmit(g_codec, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
}

bool open_i2s(const InitConfig& cfg) {
    i2s_chan_config_t chan{};
    chan.id            = I2S_NUM_0;
    chan.role          = I2S_ROLE_MASTER;
    chan.dma_desc_num  = kDmaBuffers;
    chan.dma_frame_num = kChunk;
    // Nothing new to send = zeros. Without it the DMA replays its ring, so the
    // last milliseconds of every sound repeated until the next one (heard on
    // the bench).
    chan.auto_clear_after_cb = true;
    if (i2s_new_channel(&chan, &g_tx, nullptr) != ESP_OK) return false;
    i2s_std_config_t std{};
    std.clk_cfg.sample_rate_hz  = kSampleRate;
    std.clk_cfg.clk_src         = I2S_CLK_SRC_DEFAULT;
    std.clk_cfg.mclk_multiple   = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
    std.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
    std.slot_cfg.ws_width       = 16;
    std.slot_cfg.ws_pol         = false;
    std.slot_cfg.bit_shift      = true;  // Philips: data one BCLK after WS
    std.gpio_cfg.mclk           = static_cast<gpio_num_t>(cfg.mclk_gpio);
    std.gpio_cfg.bclk           = static_cast<gpio_num_t>(cfg.bclk_gpio);
    std.gpio_cfg.ws             = static_cast<gpio_num_t>(cfg.ws_gpio);
    std.gpio_cfg.dout           = static_cast<gpio_num_t>(cfg.dout_gpio);
    std.gpio_cfg.din            = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(g_tx, &std) != ESP_OK || i2s_channel_enable(g_tx) != ESP_OK) {
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return false;
    }
    return true;
}

void close_i2s() {
    i2s_channel_disable(g_tx);
    i2s_del_channel(g_tx);
    g_tx = nullptr;
}

bool write_frames(const int16_t* buf, uint32_t n) {
    size_t written = 0;
    return i2s_channel_write(g_tx, buf, n * 4, &written, 1000) == ESP_OK && written == n * 4;
}

bool write_silence(uint32_t frames) {
    static const int16_t zeros[kChunk * 2] = {};
    for (uint32_t done = 0; done < frames; done += kChunk)
        if (!write_frames(zeros, frames - done < kChunk ? frames - done : kChunk)) return false;
    return true;
}

// Streams one sound. A newer request cuts it short: a 2 ms fade from where it
// is, then the task plays the new one (missing the end of a knob tick beats
// hearing a backlog after the knob stopped). The DMA zeroes what it has sent,
// so nothing is left to repeat once the sound ends.
bool render(const Cmd& c) {
    static int16_t buf[kChunk * 2];
    const float gain     = g_norm[static_cast<size_t>(c.sound)] * synth::volume_gain(c.volume);
    const uint32_t total = synth::length(c.sound, c.ms);
    uint32_t fade_from   = 0;  // 0 = not fading
    for (uint32_t done = 0; done < total;) {
        if (!fade_from && uxQueueMessagesWaiting(g_queue) > 0) fade_from = done;
        const uint32_t end  = fade_from ? fade_from + kFadeOut : total;
        const uint32_t stop = end < total ? end : total;
        if (done >= stop) break;
        const uint32_t n = stop - done < kChunk ? stop - done : kChunk;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = done + i;
            float v          = synth::sample(c.sound, k, c.freq, c.ms) * gain;
            if (fade_from) v *= static_cast<float>(end - k) / kFadeOut;
            v              = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            buf[2 * i]     = static_cast<int16_t>(v * 32767.0f);
            buf[2 * i + 1] = buf[2 * i];
        }
        if (!write_frames(buf, n)) return false;
        done += n;
    }
    return true;
}

void audio_task(void*) {
    bool unmuted = false;
    for (;;) {
        Cmd c;
        const TickType_t wait = unmuted ? pdMS_TO_TICKS(kMuteAfterMs) : portMAX_DELAY;
        if (xQueueReceive(g_queue, &c, wait) != pdTRUE) {
            if (unmuted) reg(es8311::kRegMute, es8311::kMuted);  // quiet for a while
            unmuted = false;
            continue;
        }
        // Unmuting ramps up: let that happen on silence, not on the sound
        // (the first sound after a pause came out softer).
        if (!unmuted && (unmuted = reg(es8311::kRegMute, es8311::kUnmuted)))
            write_silence(kSampleRate * kPreRollMs / 1000);
        if (!render(c)) ESP_LOGW(TAG, "I2S write failed");
    }
}

// Replaces whatever waits (a mailbox, not a queue): never a backlog.
bool queue(synth::Sound s, uint16_t freq = 0, uint16_t ms = 0) {
    if (!g_queue) return false;
    const uint8_t v = volume_now();
    if (v == 0) return false;  // speaker off
    const Cmd c{ s, freq, ms, v };
    return xQueueOverwrite(g_queue, &c) == pdTRUE;
}

}  // namespace

bool init(const InitConfig& cfg) {
    if (g_queue) return true;
    if (!cfg.i2c_bus) return false;
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = cfg.codec_addr;
    dev.scl_speed_hz    = 100'000;
    if (!g_codec && i2c_master_bus_add_device(cfg.i2c_bus, &dev, &g_codec) != ESP_OK) return false;
    // MCLK must run before the codec's clock registers are written.
    g_volume = cfg.volume;
    if (!open_i2s(cfg)) {
        ESP_LOGW(TAG, "I2S init failed: no audio");
        return false;
    }
    es8311::RegWrite seq[32];
    const size_t n = es8311::init_sequence(seq, 32);
    for (size_t i = 0; i < n; ++i) {
        if (!reg(seq[i].reg, seq[i].value)) {
            ESP_LOGW(TAG, "ES8311 at 0x%02x does not answer: no audio", cfg.codec_addr);
            close_i2s();
            return false;
        }
        if (seq[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(seq[i].delay_ms));
    }
    measure_sounds();
    g_queue = xQueueCreate(1, sizeof(Cmd));
    // Above ui_task (4) and httpd (5), below the network receivers (10): a
    // sound streams in real time and is cheap to compute.
    if (!g_queue || xTaskCreate(audio_task, "audio", 3072, nullptr, 6, nullptr) != pdPASS) {
        if (g_queue) vQueueDelete(g_queue);
        g_queue = nullptr;
        close_i2s();
        return false;
    }
    ESP_LOGI(TAG, "ES8311 ready, %u Hz", static_cast<unsigned>(kSampleRate));
    return true;
}

bool ready() {
    return g_queue != nullptr;
}

bool play_tone(uint16_t freq_hz, uint16_t ms) {
    return queue(synth::Sound::Tone, freq_hz, ms);
}

bool start_test() {
    return queue(synth::Sound::Chime);
}

bool play_boot() {
    return queue(synth::Sound::Croak);
}

void feedback(Feedback f, float level) {
    static constexpr synth::Sound kSounds[] = { synth::Sound::Tick, synth::Sound::Bump,
                                                synth::Sound::Confirm, synth::Sound::Cancel };
    // A gauge's tick: one octave up from the low bound to the high one.
    uint16_t freq = 0;
    if (f == Feedback::Tick && level >= 0.0f)
        freq = static_cast<uint16_t>(synth::kTickLowHz *
                                     std::pow(2.0f, level > 1.0f ? 1.0f : level));
    queue(kSounds[static_cast<size_t>(f)], freq);
}

}  // namespace pixfrog::audio
