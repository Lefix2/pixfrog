// The box's little sounds, synthesised sample by sample (16 kHz, -1..1).
// Pure and stateless (a sample is a function of its index): host-tested.
//
//   Tick   a knob notch: a light soft click, two partials (2.1 / 4.4 kHz)
//          dying in a few ms over a faint noise transient, played 10 dB
//          under the others (it repeats a lot). On a gauge its pitch follows
//          the value: an octave from the low bound to the high one. 25 ms.
//   Bump   an end stop: a soft rubbery thud, pitch falling 260 → 150 Hz with a
//          second partial for body. 120 ms.
//   Chime  the speaker test: three mellow FM bell notes (C5 E5 G5).
//   Tone   a diagnostic beep at a given pitch (console `audio tone`).
//   Confirm  a value or a choice taken: two quick round notes going up.
//   Cancel   backing out: two quick round notes going down.
//   Croak    the boot splash: a frog's "rib-bit", two buzzy syllables.
// Kept soft on purpose (slow-ish attacks, few high partials): the first
// versions were too incisive, the FM bells at a 3.5 ratio metallic.
//
// Every sound is drawn at its own level here; the driver normalizes them to
// one peak, then applies the volume.
#pragma once

#include <cmath>
#include <cstdint>

namespace pixfrog::audio::synth {

constexpr uint32_t kRate = 16000;
constexpr float kTwoPi   = 6.28318531f;

enum class Sound : uint8_t { Tick, Bump, Chime, Tone, Confirm, Cancel, Croak, Count };

// Deterministic white noise in -1..1 (an integer hash of the index).
inline float noise(uint32_t k) {
    uint32_t h  = k * 2654435761u;
    h          ^= h >> 15;
    h          *= 2246822519u;
    h          ^= h >> 13;
    return static_cast<float>(h & 0xFFFF) / 32768.0f - 1.0f;
}

inline float ramp(float t, float rise_s) {
    return t < rise_s ? t / rise_s : 1.0f;
}

// `freq`: the first partial (0 = the usual 2.1 kHz); gauges sweep it over an
// octave, 1.5 → 3 kHz.
constexpr float kTickHz = 2100.0f, kTickLowHz = 1500.0f;
inline float tick(uint32_t k, float freq) {
    const float t = static_cast<float>(k) / kRate;
    const float f = freq > 0.0f ? freq : kTickHz;
    return (0.60f * std::sin(kTwoPi * f * t) * std::exp(-t / 0.004f) +
            0.25f * std::sin(kTwoPi * 2.1f * f * t) * std::exp(-t / 0.002f) +
            0.08f * noise(k) * std::exp(-t / 0.0004f)) *
           ramp(t, 0.0006f);
}

inline float bump(uint32_t k) {
    const float t = static_cast<float>(k) / kRate;
    // f(t) = 150 + 110 e^(-t/20ms): its phase, integrated in closed form.
    const float phase = kTwoPi * (150.0f * t + 110.0f * 0.02f * (1.0f - std::exp(-t / 0.02f)));
    const float body  = 0.80f * std::sin(phase) + 0.15f * std::sin(2.0f * phase + 0.5f);
    return body * std::exp(-t / 0.035f) * ramp(t, 0.004f) +
           0.06f * noise(k) * std::exp(-t / 0.0008f);
}

// One bell note: FM, modulator at 3.5 × the pitch, its index and the
// amplitude decaying; faded over the last 15 ms so notes join without a click.
inline float bell(uint32_t k, float freq, uint32_t len) {
    const float t    = static_cast<float>(k) / kRate;
    const float idx  = 0.8f * std::exp(-t / 0.06f);
    const float tail = static_cast<float>(len - k) / (kRate * 0.015f);
    const float env  = std::exp(-t / 0.35f) * ramp(t, 0.008f) * (tail < 1.0f ? tail : 1.0f);
    return env * std::sin(kTwoPi * freq * t + idx * std::sin(kTwoPi * 2.0f * freq * t));
}

struct Note {
    uint16_t freq, ms;
};
constexpr Note kChime[]   = { { 523, 170 }, { 659, 170 }, { 784, 520 } };
constexpr Note kConfirm[] = { { 659, 45 }, { 988, 95 } };  // E5 B5
constexpr Note kCancel[]  = { { 587, 45 }, { 440, 95 } };  // D5 A4

// A round note: nearly a pure tone, a touch of 2nd harmonic in the attack,
// a 6 ms rise, faded over its last 15 ms.
inline float pluck(uint32_t k, float freq, uint32_t len) {
    const float t    = static_cast<float>(k) / kRate;
    const float tail = static_cast<float>(len - k) / (kRate * 0.015f);
    const float env  = std::exp(-t / 0.06f) * ramp(t, 0.006f) * (tail < 1.0f ? tail : 1.0f);
    const float w    = kTwoPi * freq * t;
    return env * (std::sin(w) + 0.12f * std::exp(-t / 0.02f) * std::sin(2.0f * w));
}

template <size_t N> constexpr uint32_t notes_length(const Note (&notes)[N]) {
    uint32_t n = 0;
    for (const auto& note : notes)
        n += kRate * note.ms / 1000;
    return n;
}

// A melody of plucked notes, each faded into the next.
template <size_t N> inline float melody(const Note (&notes)[N], uint32_t k) {
    for (const auto& note : notes) {
        const uint32_t len = kRate * note.ms / 1000;
        if (k < len) return pluck(k, note.freq, len);
        k -= len;
    }
    return 0.0f;
}

// One croak syllable at sample `k` of `len`: a glottal buzz at `f0` (rising
// 15 % over the syllable) through two formants (a frog's throat sac: ~750 Hz
// and ~1.8 kHz, where the little speaker still plays), rattled at 28 Hz, with
// a soft attack and release. Softly saturated: a buzz is all peaks, and the
// driver normalizes peaks, so this is what makes it as loud as the others.
inline float croak_syllable(uint32_t k, uint32_t len, float f0) {
    const float t    = static_cast<float>(k) / kRate;
    const float d    = static_cast<float>(len) / kRate;
    const float ph   = kTwoPi * f0 * (t + 0.15f * t * t / (2.0f * d));
    const float fnow = f0 * (1.0f + 0.15f * t / d);
    float v          = 0.0f;
    for (int h = 1; h <= 14; ++h) {
        const float f  = fnow * h;
        const float a1 = (f - 750.0f) / 280.0f, a2 = (f - 1800.0f) / 400.0f;
        v += (std::exp(-a1 * a1) + 0.35f * std::exp(-a2 * a2)) * std::sin(ph * h);
    }
    const float rattle = 0.7f + 0.3f * std::sin(kTwoPi * 28.0f * t);
    const float rel    = (d - t) / 0.03f;
    const float x      = 1.2f * v * rattle;
    return x / (1.0f + std::fabs(x)) * ramp(t, 0.008f) * (rel < 1.0f ? rel : 1.0f);
}

constexpr uint32_t kRib = kRate * 110 / 1000, kGap = kRate * 70 / 1000, kBit = kRate * 140 / 1000;

inline float croak(uint32_t k) {
    if (k < kRib) return croak_syllable(k, kRib, 120.0f);
    k -= kRib;
    if (k < kGap) return 0.0f;
    k -= kGap;
    return k < kBit ? croak_syllable(k, kBit, 135.0f) : 0.0f;
}

// The volume setting as a gain: even steps in dB, 1 % = -40 dB .. 100 % = 0 dB
// (0 = silent).
inline float volume_gain(uint8_t pct) {
    if (pct == 0) return 0.0f;
    const float db = -40.0f + 40.0f * static_cast<float>((pct > 100 ? 100 : pct) - 1) / 99.0f;
    return std::pow(10.0f, db / 20.0f);
}

// Samples in a sound (Tone: `ms` long).
inline uint32_t length(Sound s, uint16_t ms = 0) {
    switch (s) {
    case Sound::Tick: return kRate * 25 / 1000;
    case Sound::Bump: return kRate * 120 / 1000;
    case Sound::Chime: {
        uint32_t n = 0;
        for (const auto& note : kChime)
            n += kRate * note.ms / 1000;
        return n;
    }
    case Sound::Tone: return kRate * ms / 1000;
    case Sound::Confirm: return notes_length(kConfirm);
    case Sound::Cancel: return notes_length(kCancel);
    case Sound::Croak: return kRib + kGap + kBit;
    case Sound::Count: break;
    }
    return 0;
}

// Sample `k` of the sound, already at its playing level.
inline float sample(Sound s, uint32_t k, uint16_t freq = 0, uint16_t ms = 0) {
    switch (s) {
    case Sound::Tick: return 0.55f * tick(k, freq);
    case Sound::Bump: return 0.85f * bump(k);
    case Sound::Chime:
        for (const auto& note : kChime) {
            const uint32_t len = kRate * note.ms / 1000;
            if (k < len) return 0.7f * bell(k, note.freq, len);
            k -= len;
        }
        return 0.0f;
    case Sound::Tone: return 0.6f * bell(k, freq, length(Sound::Tone, ms));
    case Sound::Confirm: return melody(kConfirm, k);
    case Sound::Cancel: return melody(kCancel, k);
    case Sound::Croak: return croak(k);
    case Sound::Count: break;
    }
    return 0.0f;
}

}  // namespace pixfrog::audio::synth
