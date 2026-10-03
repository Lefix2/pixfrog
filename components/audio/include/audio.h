// On-board audio: the ES8311 codec (I2C config + I2S data) and the NS4150B
// speaker amplifier of the Waveshare ESP32-P4 DEV-KIT.
//
// The amplifier's enable (PA_CTRL) is pulled up for good on the bench board
// (R52 removed, GPIO53 belongs to LED output 8, see docs/HARDWARE.md), so the
// codec's DAC is kept muted except while something plays.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace pixfrog::audio {

struct InitConfig {
    i2c_master_bus_handle_t i2c_bus = nullptr;  // the shared bus (ui::i2c_bus())
    uint16_t codec_addr             = 0x18;
    int mclk_gpio                   = -1;
    int bclk_gpio                   = -1;
    int ws_gpio                     = -1;
    int dout_gpio                   = -1;  // to the codec's DSDIN (its DAC)
    int din_gpio                    = -1;  // from the codec's ASDOUT (unused yet)
    // The speaker volume, 0 (off) .. 100 %, read as each sound is queued (the
    // setting can change at any time). nullptr = 100 %.
    uint8_t (*volume)() = nullptr;
};

constexpr uint32_t kSampleRate = 16000;  // MCLK = 256 × fs = 4.096 MHz

// Brings up I2S, the codec (muted) and the playback task. False and stays
// down on any failure: a board without the codec simply has no sound.
bool init(const InitConfig& cfg);
bool ready();

// Everything plays from the audio task, in order; these only queue (a full
// queue drops the sound) and return at once, so the UI never waits on the
// speaker. One request waits at most: a newer one replaces it and cuts the
// sound playing short (2 ms fade), so a fast knob never leaves a backlog.
// The codec unmutes for a sound and mutes again after 10 s of quiet. At
// volume 0 nothing is queued (false).
bool play_tone(uint16_t freq_hz, uint16_t ms);  // a beep at that pitch (diagnostics)
bool start_test();                              // the speaker test chime
bool play_boot();                               // the boot splash's frog croak

// UI feedback (ui_task): a woody tick for a knob step that changed something,
// a soft thud when it hit an end (first/last item, min/max value).
// A click that took a value or opened something confirms, one that backs
// out (Back row, long press) cancels.
enum class Feedback : uint8_t { Tick, Bump, Confirm, Cancel };
// `level` (Tick only): where a gauge being edited stands, 0 (low bound) .. 1
// (high bound); its tick then rises one octave over the range. Negative = the
// usual tick.
void feedback(Feedback f, float level = -1.0f);

}  // namespace pixfrog::audio
