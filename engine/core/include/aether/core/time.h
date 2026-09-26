// aether/core/time.h — clocks and frame timing.
// FROZEN CONTRACT (ADR-0001).
#pragma once

#include "aether/core/types.h"

namespace aether {

// High-resolution seconds since process start (steady clock).
f64 now_seconds();

// Per-frame timing produced by the Application loop.
struct FrameTime {
    f64 total   = 0.0;   // seconds since start
    f32 delta   = 0.0f;  // clamped variable delta this frame
    f32 unscaled_delta = 0.0f;
    f32 fixed_delta = 1.0f / 60.0f;
    u64 frame_index = 0;
    f32 fps_smoothed = 0.0f;
};

// Scoped stopwatch for ad-hoc profiling.
class ScopedTimer {
public:
    explicit ScopedTimer(f64* out_seconds) : out_(out_seconds), start_(now_seconds()) {}
    ~ScopedTimer() { if (out_) *out_ = now_seconds() - start_; }

private:
    f64* out_;
    f64  start_;
};

} // namespace aether
