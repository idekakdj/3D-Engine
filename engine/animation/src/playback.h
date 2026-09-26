// playback.h (private) — splitting unwrapped playback intervals into clip-local segments.
// Shared by notify dispatch (clip.cpp, anim_graph_instance.cpp) and root motion extraction.
#pragma once

#include "aether/animation/clip.h"

#include <algorithm>
#include <cmath>

namespace aether::animation::detail {

// Larger time jumps are treated as teleports: only the last kMaxWrapCycles cycles are visited.
inline constexpr i64 kMaxWrapCycles = 64;

// Calls fn(f32 a, f32 b, bool include_end) for each monotonic clip-local piece of the unwrapped
// interval from -> to, in playback order. A piece covers [a, b) when b > a and (b, a] when b < a
// (start included, end excluded); `include_end` is set when the piece stops on a Clamp boundary
// (the playhead rests there, so an event exactly at that end must fire now).
template <typename Fn>
void for_each_segment(f64 from, f64 to, f32 duration, WrapMode mode, Fn&& fn) {
    if (!(duration > 0.0f) || from == to) {
        return;
    }
    const f64  d = static_cast<f64>(duration);
    const bool forward = to > from;
    if (mode == WrapMode::Clamp) {
        const f64  a = std::clamp(from, 0.0, d);
        const f64  b = std::clamp(to, 0.0, d);
        const bool end = forward ? (to >= d && from < d) : (to <= 0.0 && from > 0.0);
        if (a != b || end) {
            fn(static_cast<f32>(a), static_cast<f32>(b), end);
        }
        return;
    }
    // Forward pieces live in [c*d, (c+1)*d), backward pieces in (c*d, (c+1)*d].
    const i64 step = forward ? 1 : -1;
    i64 c0 = forward ? static_cast<i64>(std::floor(from / d)) : static_cast<i64>(std::ceil(from / d)) - 1;
    const i64 c1 = forward ? static_cast<i64>(std::floor(to / d)) : static_cast<i64>(std::ceil(to / d)) - 1;
    f64 start = from;
    if ((c1 - c0) * step > kMaxWrapCycles) {
        c0 = c1 - step * kMaxWrapCycles;
        start = forward ? static_cast<f64>(c0) * d : static_cast<f64>(c0 + 1) * d;
    }
    for (i64 c = c0;; c += step) {
        const f64 base = static_cast<f64>(c) * d;
        const f64 ua = (c == c0) ? start : (forward ? base : base + d);
        const f64 ub = (c == c1) ? to : (forward ? base + d : base);
        f64       la = std::clamp(ua - base, 0.0, d);
        f64       lb = std::clamp(ub - base, 0.0, d);
        if (mode == WrapMode::PingPong && (c % 2) != 0) { // odd legs play backwards
            la = d - la;
            lb = d - lb;
        }
        if (la != lb) {
            fn(static_cast<f32>(la), static_cast<f32>(lb), false);
        }
        if (c == c1) {
            break;
        }
    }
}

[[nodiscard]] inline bool notify_in_segment(f32 n, f32 a, f32 b, bool include_end) noexcept {
    if (b > a) {
        return n >= a && (n < b || (include_end && n <= b));
    }
    if (b < a) {
        return n <= a && (n > b || (include_end && n >= b));
    }
    return include_end && n == a;
}

// Calls fn(const AnimNotify&) for every notify crossed moving from -> to (unwrapped).
template <typename Fn>
void for_each_crossed_notify(const AnimationClip& clip, f64 from, f64 to, WrapMode mode, Fn&& fn) {
    const auto notifies = clip.notifies();
    if (notifies.empty()) {
        return;
    }
    const f32  d = clip.duration();
    const bool loop = mode == WrapMode::Loop;
    for_each_segment(from, to, d, mode, [&](f32 a, f32 b, bool include_end) {
        if (b >= a) {
            // Loop: a notify at `duration` is the same instant as 0 (fires as the cycle begins).
            if (loop && a <= 0.0f) {
                for (const AnimNotify& n : notifies) {
                    if (n.time >= d) {
                        fn(n);
                    }
                }
            }
            for (const AnimNotify& n : notifies) {
                if (!(loop && n.time >= d) && notify_in_segment(n.time, a, b, include_end)) {
                    fn(n);
                }
            }
        } else {
            if (loop && a >= d) {
                for (auto it = notifies.rbegin(); it != notifies.rend(); ++it) {
                    if (it->time <= 0.0f) {
                        fn(*it);
                    }
                }
            }
            for (auto it = notifies.rbegin(); it != notifies.rend(); ++it) {
                if (!(loop && it->time <= 0.0f) && notify_in_segment(it->time, a, b, include_end)) {
                    fn(*it);
                }
            }
        }
    });
}

} // namespace aether::animation::detail
