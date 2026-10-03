// ES8311 bring-up as register writes, for 16-bit Philips I2S, codec as I2S
// slave, MCLK = 256 × 16 kHz from its MCLK pin. Same sequence as Espressif's
// es8311 component (es8311_init / clock / format / power), written out
// against the reset values so it needs no register reads. Pure: host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pixfrog::audio::es8311 {

struct RegWrite {
    uint8_t reg;
    uint8_t value;
    uint16_t delay_ms;  // wait after this write
};

constexpr uint8_t kRegReset  = 0x00;
constexpr uint8_t kRegVolume = 0x32;  // DAC volume, 0xBF = 0 dB
constexpr uint8_t kRegMute   = 0x31;  // bits 6:5 = DAC soft mute
constexpr uint8_t kMuted     = 0x60;
constexpr uint8_t kUnmuted   = 0x00;

// The DAC stays at 0 dB: the volume is applied to the samples (audio.cpp),
// so no setting can push a sound into clipping.
constexpr uint8_t kVolume0dB = 0xBF;

// Writes the sequence into `out` (capacity `cap`), returns the count.
inline size_t init_sequence(RegWrite* out, size_t cap) {
    const RegWrite seq[] = {
        { kRegReset, 0x1F, 20 },  // reset
        { kRegReset, 0x00, 0 },
        { kRegReset, 0x80, 0 },  // power on, I2S slave
        { 0x01, 0x3F, 0 },       // every clock on, MCLK from the MCLK pin
        { 0x02, 0x00, 0 },       // pre-divider 1, multiplier 1
        { 0x05, 0x00, 0 },       // ADC / DAC dividers 1
        { 0x03, 0x10, 0 },       // single speed, ADC OSR
        { 0x04, 0x10, 0 },       // DAC OSR
        { 0x07, 0x00, 0 },       // LRCK = MCLK / 256 (high bits)
        { 0x08, 0xFF, 0 },       // (low bits)
        { 0x06, 0x03, 0 },       // BCLK = MCLK / 4, not inverted
        { 0x09, 0x0C, 0 },       // DAC input: I2S, 16 bit
        { 0x0A, 0x0C, 0 },       // ADC output: I2S, 16 bit
        { 0x0D, 0x01, 0 },       // analog power up
        { 0x0E, 0x02, 0 },       // PGA + ADC modulator on
        { 0x12, 0x00, 0 },       // DAC power up
        { 0x13, 0x10, 0 },       // output to the headphone / line driver
        { 0x1C, 0x6A, 0 },       // ADC EQ bypass, DC offset cancel
        { 0x37, 0x08, 0 },       // DAC EQ bypass
        { kRegVolume, kVolume0dB, 0 },
        { kRegMute, kMuted, 0 },  // silent until something plays
    };
    const size_t n = sizeof(seq) / sizeof(seq[0]);
    if (cap < n) return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = seq[i];
    return n;
}

}  // namespace pixfrog::audio::es8311
