// aether/gameplay/input_subsystem.h — per-frame input mapping as an engine subsystem.
//
// InputSubsystem (SubsystemKind::Engine, registered first among the default subsystems)
// snapshots core's Input::state() in on_begin_frame, masks what Dear ImGui is capturing
// (keyboard while a text field is active, mouse while hovering/dragging an ImGui window)
// and evaluates its InputMap. Gameplay code, camera controllers and scripts then query
// `input_map()` / `state()` for the rest of the frame.
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/input.h"
#include "aether/core/subsystem.h"
#include "aether/gameplay/input_map.h"

namespace aether::gameplay {

class InputSubsystem final : public ISubsystem {
public:
    InputSubsystem();

    [[nodiscard]] const char* name() const override { return "Input"; }
    void on_begin_frame(FrameContext& ctx) override;

    [[nodiscard]] InputMap&       input_map() { return map_; }
    [[nodiscard]] const InputMap& input_map() const { return map_; }

    // This frame's (ImGui-masked) input snapshot.
    [[nodiscard]] const InputState& state() const { return state_; }

    // When true (default), input captured by ImGui is hidden from the map.
    void set_respect_imgui_capture(bool respect) { respect_imgui_ = respect; }
    [[nodiscard]] bool respect_imgui_capture() const { return respect_imgui_; }

    // Evaluates `raw` exactly like on_begin_frame does (tests, replays, custom hosts).
    void feed(const InputState& raw, bool imgui_wants_keyboard, bool imgui_wants_mouse);

private:
    InputMap   map_;
    InputState state_{};
    bool       respect_imgui_ = true;
};

} // namespace aether::gameplay
