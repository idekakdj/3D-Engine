// time.cpp — process-relative high resolution clock.
#include "aether/core/time.h"

#include <chrono>

namespace aether {
namespace {

using Clock = std::chrono::steady_clock;

// Function-local static: valid even when now_seconds() is first called from another
// translation unit's static initializer.
Clock::time_point epoch() {
    static const Clock::time_point start = Clock::now();
    return start;
}

// Pin the epoch as early as possible (this TU's dynamic initialization), so "seconds
// since process start" is accurate even if nobody asks for the time until much later.
[[maybe_unused]] const Clock::time_point g_pin_epoch = epoch();

} // namespace

f64 now_seconds() {
    return std::chrono::duration<f64>(Clock::now() - epoch()).count();
}

} // namespace aether
