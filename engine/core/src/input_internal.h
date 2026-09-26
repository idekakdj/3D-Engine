// input_internal.h — private hooks through which the Window feeds raw events into the
// global InputState (main thread only). Codes are GLFW values (Key mirrors them).
#pragma once

#include "aether/core/input.h"
#include "aether/core/types.h"

namespace aether::detail {

// Edge rules: press -> Pressed (repeat events ignored); release -> Released. A key
// pressed AND released within one frame reports Pressed for that frame, then Released
// on the next begin_frame() (quick taps are never lost). Out-of-range codes are ignored.
void input_on_key(i32 key, bool pressed);
void input_on_mouse_button(i32 button, bool pressed);
void input_on_cursor(f64 x, f64 y); // accumulates cursor_delta (first sample: no delta)
void input_on_scroll(f64 dx, f64 dy);
void input_reset(); // everything Up, deltas cleared (tests / focus changes)

} // namespace aether::detail
