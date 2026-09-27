// anim_view.cpp — see anim_view.h.
#include "aether/editor/anim_view.h"

#include "aether/animation/anim_graph.h"

namespace aether::editor {

AnimatorView describe_animator(const animation::AnimGraphInstance& instance) {
    AnimatorView view;
    if (!instance.valid() || instance.graph() == nullptr) {
        return view;
    }
    const animation::AnimGraph& graph = *instance.graph();
    view.valid      = true;
    view.node_count = graph.node_count();
    view.root       = graph.root();

    for (u32 p = 0; p < graph.parameter_count(); ++p) {
        AnimParamView pv;
        pv.name = graph.parameter_name(p);
        switch (graph.parameter_type(p)) {
        case animation::ParamType::Float:
            pv.kind  = AnimParamKind::Float;
            pv.value = instance.get_float(p);
            break;
        case animation::ParamType::Bool:
            pv.kind  = AnimParamKind::Bool;
            pv.value = instance.get_bool(p) ? 1.0f : 0.0f;
            break;
        case animation::ParamType::Trigger:
            pv.kind  = AnimParamKind::Trigger;
            pv.value = instance.is_trigger_set(p) ? 1.0f : 0.0f;
            break;
        }
        view.parameters.push_back(std::move(pv));
    }

    for (u32 node = 0; node < graph.node_count(); ++node) {
        const u32 count = graph.state_count(node);
        if (count == 0) {
            continue; // not a state machine
        }
        AnimStateMachineView sm;
        sm.node          = node;
        sm.current       = instance.current_state(node);
        sm.in_transition = instance.in_transition(node);
        sm.source        = sm.in_transition ? instance.transition_source(node) : animation::kInvalidState;
        sm.alpha         = instance.transition_alpha(node);
        sm.weight        = instance.node_weight(node);
        for (u32 s = 0; s < count; ++s) {
            AnimStateView st;
            st.id                = s;
            st.current           = s == sm.current;
            st.transition_source = sm.in_transition && s == sm.source;
            st.normalized_time   = instance.state_normalized_time(node, s);
            sm.states.push_back(st);
        }
        view.machines.push_back(std::move(sm));
    }
    return view;
}

} // namespace aether::editor
