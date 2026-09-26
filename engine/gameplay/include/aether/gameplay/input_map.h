// aether/gameplay/input_map.h — named input actions and axes (Unreal-style action mapping).
//
// An InputMap binds NAMES to physical inputs and is evaluated once per frame against an
// InputState snapshot (aether/core/input.h):
//   * actions — digital: down while ANY bound key / mouse button is down. pressed() and
//     released() are edges relative to the previous update() (so multiple bindings of one
//     action never produce duplicate edges).
//   * axes    — analog: the SUM of every binding's contribution this frame:
//       key / mouse button : `scale` while held, else 0      (W = +1, S = -1 -> MoveForward)
//       mouse delta x / y  : cursor delta in pixels * scale
//       scroll x / y       : wheel delta this frame * scale
//
// JSON format (load_json / to_json):
//   {
//     "actions": { "Jump": ["Space"], "Fire": ["Mouse:Left", "LeftControl"] },
//     "axes": {
//       "MoveForward": [ { "input": "W", "scale": 1 }, { "input": "S", "scale": -1 } ],
//       "LookX":       [ { "input": "MouseDeltaX", "scale": 1 } ],
//       "Zoom":        [ { "input": "ScrollY" } ]                       // scale defaults to 1
//     }
//   }
// Input names: key names as returned by key_name() ("A".."Z", "Num0".."Num9", "Space",
// "LeftShift", "F1", "Escape", ...), "Mouse:Left|Right|Middle|Button4|Button5",
// "MouseDeltaX", "MouseDeltaY", "ScrollX", "ScrollY". Names are case-insensitive on load.
//
// Thread-affinity: an InputMap is plain data; one thread at a time (normally main).
#pragma once

#include "aether/core/error.h"
#include "aether/core/input.h"
#include "aether/core/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aether::gameplay {

enum class InputSourceKind : u8 {
    Key = 0,
    MouseButton,
    MouseDeltaX,
    MouseDeltaY,
    ScrollX,
    ScrollY,
};

struct InputBinding {
    InputSourceKind kind = InputSourceKind::Key;
    Key             key = Key::Unknown;           // kind == Key
    MouseButton     button = MouseButton::Left;   // kind == MouseButton
    f32             scale = 1.0f;                 // axes only

    [[nodiscard]] static InputBinding from_key(Key k, f32 scale = 1.0f) {
        InputBinding b;
        b.kind = InputSourceKind::Key;
        b.key = k;
        b.scale = scale;
        return b;
    }
    [[nodiscard]] static InputBinding from_mouse_button(MouseButton mb, f32 scale = 1.0f) {
        InputBinding b;
        b.kind = InputSourceKind::MouseButton;
        b.button = mb;
        b.scale = scale;
        return b;
    }
    [[nodiscard]] static InputBinding from_source(InputSourceKind kind, f32 scale = 1.0f) {
        InputBinding b;
        b.kind = kind;
        b.scale = scale;
        return b;
    }
    friend bool operator==(const InputBinding&, const InputBinding&) = default;
};

class InputMap {
public:
    // ---- bindings ------------------------------------------------------------------------
    void bind_action(StringView action, Key key);
    void bind_action(StringView action, MouseButton button);
    void bind_action(StringView action, const InputBinding& binding);

    void bind_axis(StringView axis, Key key, f32 scale);
    void bind_axis(StringView axis, MouseButton button, f32 scale);
    void bind_axis(StringView axis, InputSourceKind source, f32 scale = 1.0f); // mouse delta / scroll
    void bind_axis(StringView axis, const InputBinding& binding);

    // Removes an action or axis (and its state). Returns true if it existed.
    bool unbind(StringView name);
    void clear();

    [[nodiscard]] bool has_action(StringView action) const;
    [[nodiscard]] bool has_axis(StringView axis) const;
    [[nodiscard]] const std::vector<InputBinding>* action_bindings(StringView action) const;
    [[nodiscard]] const std::vector<InputBinding>* axis_bindings(StringView axis) const;
    [[nodiscard]] std::vector<std::string> action_names() const; // sorted
    [[nodiscard]] std::vector<std::string> axis_names() const;   // sorted

    // ---- per-frame evaluation --------------------------------------------------------------
    // Evaluates every action/axis against `state`. Call once per frame.
    void update(const InputState& state);
    // Releases every action (edges fire on the next update) and zeroes axes; e.g. on focus loss.
    void reset_state();

    [[nodiscard]] bool action_down(StringView action) const;
    [[nodiscard]] bool action_pressed(StringView action) const;  // went down this update
    [[nodiscard]] bool action_released(StringView action) const; // went up this update
    [[nodiscard]] f32  axis(StringView axis) const;              // 0 for unknown axes

    // ---- serialization -----------------------------------------------------------------------
    // Replaces all bindings. Unknown input names are skipped with a warning (counted in the
    // return value); malformed documents fail and leave the map unchanged.
    Result<usize> load_json(StringView json);
    Result<usize> load_file(const std::filesystem::path& file);
    [[nodiscard]] std::string to_json() const;

    // The default editor-style camera bindings used by the camera controllers
    // (camera_controller.h): Camera.MoveForward (W/S, Up/Down), Camera.MoveRight (D/A),
    // Camera.MoveUp (E/Q), Camera.LookX/LookY (mouse delta), Camera.Zoom (scroll Y),
    // Camera.Look (right mouse), Camera.Pan (middle mouse), Camera.Boost (left shift).
    void add_default_camera_bindings();

private:
    struct Entry {
        std::string               name;
        std::vector<InputBinding> bindings;
        bool                      down = false;
        bool                      prev_down = false;
        f32                       value = 0.0f;
    };
    Entry*       find(std::vector<Entry>& list, StringView name);
    const Entry* find(const std::vector<Entry>& list, StringView name) const;
    Entry&       find_or_add(std::vector<Entry>& list, StringView name);

    std::vector<Entry> actions_;
    std::vector<Entry> axes_;
};

// Key / input names (see the file comment). key_name(Key::Unknown) == "Unknown".
[[nodiscard]] StringView         key_name(Key key);
[[nodiscard]] std::optional<Key> key_from_name(StringView name); // case-insensitive
[[nodiscard]] std::string        binding_input_name(const InputBinding& binding);
[[nodiscard]] std::optional<InputBinding> binding_from_input_name(StringView name, f32 scale = 1.0f);

} // namespace aether::gameplay
