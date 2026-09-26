// aether/gameplay/fixed_step.h — the fixed-step accumulator driving on_fixed_update.
//
// Blueprint §5.1: the frame's (clamped) variable delta is accumulated; every whole
// `fixed_delta` in the accumulator is one fixed step. At most `max_steps` steps run per
// frame (spiral-of-death clamp); when the clamp hits, the excess time is DROPPED so the
// simulation slows down instead of falling ever further behind. After advance() the
// remainder is always in [0, fixed_delta) and alpha() = remainder / fixed_delta is the
// render interpolation factor between the previous and the latest simulated state.
//
// Header-only, plain arithmetic; not thread-safe (one instance per loop).
#pragma once

#include "aether/core/types.h"

#include <algorithm>
#include <cmath>

namespace aether::gameplay {

class FixedStepAccumulator {
public:
    FixedStepAccumulator() = default;
    FixedStepAccumulator(f32 fixed_delta, u32 max_steps) { configure(fixed_delta, max_steps); }

    void configure(f32 fixed_delta, u32 max_steps) {
        fixed_delta_ = fixed_delta > 0.0f ? fixed_delta : 1.0f / 60.0f;
        max_steps_   = max_steps;
        accumulator_ = std::min(accumulator_, static_cast<f64>(fixed_delta_));
    }

    // Adds `frame_delta` seconds (negative values count as 0) and returns how many fixed
    // steps to run now (<= max_steps). Excess time beyond max_steps is discarded.
    [[nodiscard]] u32 advance(f32 frame_delta) {
        const f64 fixed = static_cast<f64>(fixed_delta_);
        accumulator_ += static_cast<f64>(std::max(frame_delta, 0.0f));
        // Tolerate float noise: 0.9999999 of a step counts as a step (avoids 0/2 step jitter
        // when frame_delta == fixed_delta converted through f32).
        constexpr f64 kEpsilon = 1e-9;
        u32 steps = 0;
        while (accumulator_ + kEpsilon >= fixed && steps < max_steps_) {
            accumulator_ -= fixed;
            ++steps;
        }
        if (accumulator_ < 0.0) {
            accumulator_ = 0.0;
        }
        if (accumulator_ + kEpsilon >= fixed) {
            dropped_ += std::floor((accumulator_ + kEpsilon) / fixed); // whole steps discarded
            accumulator_ = std::fmod(accumulator_, fixed);
            if (accumulator_ + kEpsilon >= fixed) {
                accumulator_ = 0.0;
            }
        }
        total_steps_ += steps;
        return steps;
    }

    // Discards the accumulated remainder (world reload, resuming from pause).
    void reset() {
        accumulator_ = 0.0;
    }

    [[nodiscard]] f32 alpha() const {
        return static_cast<f32>(std::clamp(accumulator_ / static_cast<f64>(fixed_delta_), 0.0, 1.0));
    }
    [[nodiscard]] f64 remainder() const { return accumulator_; }
    [[nodiscard]] f32 fixed_delta() const { return fixed_delta_; }
    [[nodiscard]] u32 max_steps() const { return max_steps_; }
    [[nodiscard]] u64 total_steps() const { return total_steps_; }
    // Whole steps discarded by the spiral-of-death clamp since construction.
    [[nodiscard]] f64 dropped_steps() const { return dropped_; }

private:
    f64 accumulator_ = 0.0;
    f32 fixed_delta_ = 1.0f / 60.0f;
    u32 max_steps_   = 8;
    u64 total_steps_ = 0;
    f64 dropped_     = 0.0;
};

} // namespace aether::gameplay
