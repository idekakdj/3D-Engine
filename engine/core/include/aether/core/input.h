// aether/core/input.h — keyboard/mouse state and the action mapping surface.
//
// FROZEN CONTRACT (ADR-0001). Key codes mirror GLFW values but are declared here so
// no engine code above core includes GLFW. The Window feeds raw events into an
// InputState each frame; gameplay/scripts query it or subscribe to mapped actions.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

namespace aether {

enum class Key : u16 {
    Unknown = 0,
    Space = 32, Apostrophe = 39, Comma = 44, Minus, Period, Slash,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket = 91, Backslash, RightBracket, GraveAccent = 96,
    Escape = 256, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up, PageUp, PageDown, Home, End,
    CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift = 340, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    Count = 512,
};

enum class MouseButton : u8 { Left = 0, Right, Middle, Button4, Button5, Count };

enum class ButtonState : u8 { Up = 0, Pressed /*edge*/, Held, Released /*edge*/ };

// Snapshot of input for one frame. Owned by core; produced by the Window.
struct InputState {
    ButtonState keys[static_cast<usize>(Key::Count)]{};
    ButtonState mouse[static_cast<usize>(MouseButton::Count)]{};
    Vec2        cursor{ 0.0f };        // pixels, top-left origin
    Vec2        cursor_delta{ 0.0f };
    Vec2        scroll{ 0.0f };        // wheel delta this frame

    [[nodiscard]] bool key_down(Key k) const {
        auto s = keys[static_cast<usize>(k)];
        return s == ButtonState::Pressed || s == ButtonState::Held;
    }
    [[nodiscard]] bool key_pressed(Key k) const {
        return keys[static_cast<usize>(k)] == ButtonState::Pressed;
    }
    [[nodiscard]] bool mouse_down(MouseButton b) const {
        auto s = mouse[static_cast<usize>(b)];
        return s == ButtonState::Pressed || s == ButtonState::Held;
    }
};

// Input singleton accessor (valid after Window creation). Thread-affinity: main.
class Input {
public:
    static const InputState& state();
    // Advance edge states (Pressed->Held, Released->Up) at frame start. Called by
    // the Window/Application; not for gameplay use.
    static void begin_frame();
};

} // namespace aether
