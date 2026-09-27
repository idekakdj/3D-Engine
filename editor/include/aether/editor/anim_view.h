// aether/editor/anim_view.h — read-only view model of an animator's graph (state machines,
// states, transition progress, parameters) for the editor's animation panel.
//
// Built only from the animation module's public introspection API. That API has no state
// names by id, no transition list and no node kinds, so: state machines are discovered as the
// nodes with state_count() > 0, states are listed by index, and transitions are shown only as
// the live "source -> current (alpha)" blend. See the M2 editor report for the requested API.
//
// Main thread only.
#pragma once

#include "aether/core/types.h"

#include <string>
#include <vector>

namespace aether::animation {
class AnimGraphInstance;
}

namespace aether::editor {

struct AnimStateView {
    u32  id                = 0;
    bool current           = false;
    bool transition_source = false;
    f64  normalized_time   = 0.0; // of this state's playback
};

struct AnimStateMachineView {
    u32                        node          = 0;
    u32                        current       = 0xFFFF'FFFFu; // kInvalidState when none
    bool                       in_transition = false;
    u32                        source        = 0xFFFF'FFFFu;
    f32                        alpha         = 1.0f; // transition progress (1 when settled)
    f32                        weight        = 0.0f; // node weight in the last update
    std::vector<AnimStateView> states;
};

enum class AnimParamKind : u8 { Float = 0, Bool, Trigger };

struct AnimParamView {
    std::string   name;
    AnimParamKind kind  = AnimParamKind::Float;
    f32           value = 0.0f; // float value; bool / trigger-set as 0 / 1
};

struct AnimatorView {
    bool                              valid      = false;
    u32                               node_count = 0;
    u32                               root       = 0xFFFF'FFFFu;
    std::vector<AnimParamView>        parameters;
    std::vector<AnimStateMachineView> machines; // in node order
};

[[nodiscard]] AnimatorView describe_animator(const animation::AnimGraphInstance& instance);

} // namespace aether::editor
