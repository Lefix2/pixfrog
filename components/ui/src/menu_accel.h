// Rotation acceleration for value editors — pure logic, so the host tests can
// drive it with their own clock.
//
// Sustained rotation in one direction escalates the per-detent step ×10, then
// ×100. A reversal starts over at ×1 (it is a correction: fine steps back
// over the overshoot); a pause drops back too — ×100 after a short one, so a
// brief hesitation at high speed does not keep the big step, everything after
// a longer one. Tuned for ~20 detents/s on a fast flick: ×10 after about half
// a turn, ×100 after roughly two seconds of continuous spinning.
#pragma once

#include <cstdint>

namespace pixfrog::ui::detail {

struct RotationAccel {
    static constexpr uint32_t kResetMs        = 350;  // any pause this long: ×1
    static constexpr uint32_t kHundredsHoldMs = 150;  // ×100 needs detents this close
    static constexpr uint16_t kTensAt         = 10;
    static constexpr uint16_t kHundredsAt     = 36;

    uint32_t last_ms = 0;
    uint16_t streak  = 0;
    int8_t dir       = 0;  // +1 right, -1 left, 0 = none yet

    void reset() {
        streak = 0;
        dir    = 0;
    }

    // One detent at `now_ms`, rotating right or left: the step multiplier.
    int32_t note(uint32_t now_ms, bool right) {
        const int8_t d     = right ? 1 : -1;
        const uint32_t gap = now_ms - last_ms;
        if (d != dir || gap > kResetMs)
            streak = 0;
        else if (streak >= kHundredsAt && gap > kHundredsHoldMs)
            streak = kTensAt;  // slowed down at ×100: back to ×10, not ×1
        if (streak < UINT16_MAX) ++streak;
        dir     = d;
        last_ms = now_ms;
        if (streak >= kHundredsAt) return 100;
        if (streak >= kTensAt) return 10;
        return 1;
    }
};

}  // namespace pixfrog::ui::detail
