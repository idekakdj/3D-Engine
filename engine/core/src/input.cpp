// input.cpp — global InputState with correct per-frame edge semantics.
#include "input_internal.h"

#include <bitset>

namespace aether {
namespace {

constexpr usize kKeyCount   = static_cast<usize>(Key::Count);
constexpr usize kMouseCount = static_cast<usize>(MouseButton::Count);

struct InputGlobals {
    InputState                state{};
    std::bitset<kKeyCount>    key_release_pending;   // pressed+released in one frame
    std::bitset<kMouseCount>  mouse_release_pending;
    bool                      have_cursor = false;
};

InputGlobals& globals() {
    static InputGlobals g;
    return g;
}

void on_button(ButtonState& s, bool pressed, bool& release_pending) {
    if (pressed) {
        if (s == ButtonState::Up || s == ButtonState::Released) {
            s = ButtonState::Pressed;
        }
        release_pending = false;
    } else {
        if (s == ButtonState::Pressed) {
            release_pending = true; // keep the press visible this frame
        } else if (s == ButtonState::Held) {
            s = ButtonState::Released;
        }
    }
}

void advance(ButtonState& s, bool& release_pending) {
    switch (s) {
    case ButtonState::Pressed: s = release_pending ? ButtonState::Released : ButtonState::Held; break;
    case ButtonState::Released: s = ButtonState::Up; break;
    case ButtonState::Held:
    case ButtonState::Up: break;
    }
    release_pending = false;
}

} // namespace

const InputState& Input::state() {
    return globals().state;
}

void Input::begin_frame() {
    InputGlobals& g = globals();
    for (usize i = 0; i < kKeyCount; ++i) {
        bool pending = g.key_release_pending[i];
        advance(g.state.keys[i], pending);
        g.key_release_pending[i] = pending;
    }
    for (usize i = 0; i < kMouseCount; ++i) {
        bool pending = g.mouse_release_pending[i];
        advance(g.state.mouse[i], pending);
        g.mouse_release_pending[i] = pending;
    }
    g.state.cursor_delta = Vec2(0.0f);
    g.state.scroll       = Vec2(0.0f);
}

namespace detail {

void input_on_key(i32 key, bool pressed) {
    if (key < 0 || static_cast<usize>(key) >= kKeyCount) {
        return;
    }
    InputGlobals& g       = globals();
    bool          pending = g.key_release_pending[static_cast<usize>(key)];
    on_button(g.state.keys[static_cast<usize>(key)], pressed, pending);
    g.key_release_pending[static_cast<usize>(key)] = pending;
}

void input_on_mouse_button(i32 button, bool pressed) {
    if (button < 0 || static_cast<usize>(button) >= kMouseCount) {
        return;
    }
    InputGlobals& g       = globals();
    bool          pending = g.mouse_release_pending[static_cast<usize>(button)];
    on_button(g.state.mouse[static_cast<usize>(button)], pressed, pending);
    g.mouse_release_pending[static_cast<usize>(button)] = pending;
}

void input_on_cursor(f64 x, f64 y) {
    InputGlobals& g = globals();
    const Vec2    p(static_cast<f32>(x), static_cast<f32>(y));
    if (g.have_cursor) {
        g.state.cursor_delta += p - g.state.cursor;
    }
    g.state.cursor = p;
    g.have_cursor  = true;
}

void input_on_scroll(f64 dx, f64 dy) {
    globals().state.scroll += Vec2(static_cast<f32>(dx), static_cast<f32>(dy));
}

void input_reset() {
    InputGlobals& g = globals();
    g               = InputGlobals{};
}

} // namespace detail
} // namespace aether
