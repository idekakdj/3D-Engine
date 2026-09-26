// anim_graph_internal.h (private) — the immutable AnimGraph definition shared by the builder and
// the runtime instance.
#pragma once

#include "aether/animation/anim_graph.h"
#include "aether/animation/skeleton.h"
#include "blend_space.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::animation::detail {

enum class NodeType : u8 { Clip = 0, Blend1D, Blend2D, Additive, MaskedLayer, StateMachine };

inline constexpr StateId kAnyState = kInvalidState - 1;

struct ParamDef {
    std::string name;
    ParamType   type = ParamType::Float;
    f32         default_value = 0.0f;
};

struct ClipNodeDef {
    std::shared_ptr<const AnimationClip> clip;
    f32         speed = 1.0f;
    WrapMode    wrap = WrapMode::Loop;
    ParamId     speed_param = kInvalidParam;
    SyncGroupId sync_group = kNoSyncGroup;
    f32         start_time = 0.0f;
};

struct Blend1DDef {
    ParamId          param = kInvalidParam;
    std::vector<f32> thresholds; // ascending, parallel to NodeDef::children
};

struct Blend2DDef {
    ParamId           param_x = kInvalidParam;
    ParamId           param_y = kInvalidParam;
    std::vector<Vec2> points; // parallel to NodeDef::children
    BlendTriangulation triangulation;
};

struct AdditiveDef {
    f32                    weight = 1.0f;
    ParamId                weight_param = kInvalidParam;
    std::vector<Transform> reference;
    BoneMask               mask; // empty = all joints
};

struct LayerDef {
    f32      weight = 1.0f;
    ParamId  weight_param = kInvalidParam;
    BoneMask mask;
};

struct StateDef {
    std::string name;
    NodeId      node = kInvalidNode;
    bool        reset_on_enter = true;
};

struct TransitionDef {
    StateId                          from = kAnyState;
    StateId                          to = kInvalidState;
    std::vector<TransitionCondition> conditions;
    TransitionOptions                options;
};

struct StateMachineDef {
    std::vector<StateDef>         states;
    std::vector<TransitionDef>    transitions;
    std::vector<u32>              any_state;  // transition indices, declaration order
    std::vector<std::vector<u32>> outgoing;   // per state: transition indices
    StateId                       entry = kInvalidState;
};

struct NodeDef {
    NodeType            type = NodeType::Clip;
    u32                 payload = 0; // index into the per-type array
    std::vector<NodeId> children;    // Additive/Masked: {base, layer}; SM: state nodes
};

struct AnimGraphDef {
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<NodeDef>            nodes;
    std::vector<ClipNodeDef>        clip_nodes;
    std::vector<Blend1DDef>         blend1d;
    std::vector<Blend2DDef>         blend2d;
    std::vector<AdditiveDef>        additive;
    std::vector<LayerDef>           layers;
    std::vector<StateMachineDef>    state_machines;
    std::vector<ParamDef>           params;
    StringMap<ParamId>              param_lookup;
    std::vector<std::string>        sync_groups;
    StringMap<SyncGroupId>          sync_lookup;
    std::vector<std::vector<NodeId>> sync_members; // per group: clip nodes
    NodeId                          root = kInvalidNode;
    u32                             pose_levels = 0; // scratch poses needed by evaluation
    f32                             notify_weight_threshold = 0.0f;
};

} // namespace aether::animation::detail
