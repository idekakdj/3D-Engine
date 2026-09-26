// anim_graph.cpp — AnimGraph accessors and AnimGraphBuilder (validation + finalization).
#include "aether/animation/anim_graph.h"

#include "aether/animation/skeleton.h"
#include "anim_graph_internal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

namespace aether::animation {

using detail::NodeType;

// ================================================================================================
// AnimGraph
// ================================================================================================
AnimGraph::AnimGraph(std::unique_ptr<detail::AnimGraphDef> def) : def_(std::move(def)) {}
AnimGraph::~AnimGraph() = default;

const Skeleton& AnimGraph::skeleton() const noexcept { return *def_->skeleton; }
const std::shared_ptr<const Skeleton>& AnimGraph::skeleton_ptr() const noexcept {
    return def_->skeleton;
}
NodeId AnimGraph::root() const noexcept { return def_->root; }
u32    AnimGraph::node_count() const noexcept { return static_cast<u32>(def_->nodes.size()); }
u32 AnimGraph::parameter_count() const noexcept { return static_cast<u32>(def_->params.size()); }

ParamId AnimGraph::find_parameter(std::string_view name) const noexcept {
    const auto it = def_->param_lookup.find(name);
    return it == def_->param_lookup.end() ? kInvalidParam : it->second;
}
const std::string& AnimGraph::parameter_name(ParamId id) const { return def_->params[id].name; }
ParamType AnimGraph::parameter_type(ParamId id) const { return def_->params[id].type; }

SyncGroupId AnimGraph::find_sync_group(std::string_view name) const noexcept {
    const auto it = def_->sync_lookup.find(name);
    return it == def_->sync_lookup.end() ? kNoSyncGroup : it->second;
}

StateId AnimGraph::find_state(NodeId sm, std::string_view name) const noexcept {
    if (sm >= def_->nodes.size() || def_->nodes[sm].type != NodeType::StateMachine) {
        return kInvalidState;
    }
    const auto& states = def_->state_machines[def_->nodes[sm].payload].states;
    for (u32 i = 0; i < states.size(); ++i) {
        if (states[i].name == name) {
            return i;
        }
    }
    return kInvalidState;
}

u32 AnimGraph::state_count(NodeId sm) const noexcept {
    if (sm >= def_->nodes.size() || def_->nodes[sm].type != NodeType::StateMachine) {
        return 0;
    }
    return static_cast<u32>(def_->state_machines[def_->nodes[sm].payload].states.size());
}

Result<std::shared_ptr<const AnimGraph>> AnimGraph::make_single_clip(
    std::shared_ptr<const Skeleton> skeleton, std::shared_ptr<const AnimationClip> clip,
    WrapMode wrap, f32 speed) {
    AnimGraphBuilder b(std::move(skeleton));
    ClipNodeOptions  options;
    options.wrap = wrap;
    options.speed = speed;
    const NodeId node = b.add_clip(std::move(clip), options);
    return b.build(node);
}

// ================================================================================================
// Builder
// ================================================================================================
struct AnimGraphBuilder::Impl {
    std::unique_ptr<detail::AnimGraphDef> def = std::make_unique<detail::AnimGraphDef>();
    std::string                           error;

    void fail(std::string message) {
        if (error.empty()) {
            error = std::move(message);
        }
    }
    [[nodiscard]] bool node_ok(NodeId n) const { return n < def->nodes.size(); }
    [[nodiscard]] u32  joints() const { return def->skeleton ? def->skeleton->joint_count() : 0u; }

    // Optional float parameter (kInvalidParam allowed).
    bool check_param(ParamId p, ParamType type, bool optional, const char* what) {
        if (optional && p == kInvalidParam) {
            return true;
        }
        if (p >= def->params.size() || def->params[p].type != type) {
            fail(std::format("AnimGraphBuilder: {} requires a {} parameter (got id {})", what,
                             type == ParamType::Float  ? "float"
                             : type == ParamType::Bool ? "bool"
                                                       : "trigger",
                             p));
            return false;
        }
        return true;
    }

    NodeId add_node(NodeType type, u32 payload, std::vector<NodeId> children) {
        detail::NodeDef nd;
        nd.type = type;
        nd.payload = payload;
        nd.children = std::move(children);
        def->nodes.push_back(std::move(nd));
        return static_cast<NodeId>(def->nodes.size() - 1);
    }

    detail::StateMachineDef* machine(NodeId sm) {
        if (!node_ok(sm) || def->nodes[sm].type != NodeType::StateMachine) {
            fail(std::format("AnimGraphBuilder: node {} is not a state machine", sm));
            return nullptr;
        }
        return &def->state_machines[def->nodes[sm].payload];
    }

    ParamId add_param(std::string name, ParamType type, f32 value) {
        if (name.empty() || def->param_lookup.contains(name)) {
            fail(std::format("AnimGraphBuilder: parameter name '{}' is empty or duplicated", name));
            return kInvalidParam;
        }
        const auto id = static_cast<ParamId>(def->params.size());
        def->param_lookup.emplace(name, id);
        def->params.push_back({std::move(name), type, value});
        return id;
    }
};

AnimGraphBuilder::AnimGraphBuilder(std::shared_ptr<const Skeleton> skeleton)
    : impl_(std::make_unique<Impl>()) {
    if (!skeleton) {
        impl_->fail("AnimGraphBuilder: null skeleton");
    }
    impl_->def->skeleton = std::move(skeleton);
}
AnimGraphBuilder::~AnimGraphBuilder() = default;
AnimGraphBuilder::AnimGraphBuilder(AnimGraphBuilder&&) noexcept = default;
AnimGraphBuilder& AnimGraphBuilder::operator=(AnimGraphBuilder&&) noexcept = default;

ParamId AnimGraphBuilder::add_float(std::string name, f32 default_value) {
    return impl_->add_param(std::move(name), ParamType::Float, default_value);
}
ParamId AnimGraphBuilder::add_bool(std::string name, bool default_value) {
    return impl_->add_param(std::move(name), ParamType::Bool, default_value ? 1.0f : 0.0f);
}
ParamId AnimGraphBuilder::add_trigger(std::string name) {
    return impl_->add_param(std::move(name), ParamType::Trigger, 0.0f);
}

SyncGroupId AnimGraphBuilder::add_sync_group(std::string name) {
    auto& def = *impl_->def;
    if (const auto it = def.sync_lookup.find(name); it != def.sync_lookup.end()) {
        return it->second; // idempotent by name
    }
    const auto id = static_cast<SyncGroupId>(def.sync_groups.size());
    def.sync_lookup.emplace(name, id);
    def.sync_groups.push_back(std::move(name));
    def.sync_members.emplace_back();
    return id;
}

void AnimGraphBuilder::set_notify_weight_threshold(f32 threshold) {
    impl_->def->notify_weight_threshold = std::max(threshold, 0.0f);
}

NodeId AnimGraphBuilder::add_clip(std::shared_ptr<const AnimationClip> clip,
                                  const ClipNodeOptions& options) {
    auto& im = *impl_;
    if (!clip) {
        im.fail("AnimGraphBuilder: add_clip with a null clip");
        return kInvalidNode;
    }
    if (clip->required_joint_count() > im.joints()) {
        im.fail(std::format("AnimGraphBuilder: clip '{}' animates joint {} but the skeleton has {}",
                            clip->name(), clip->required_joint_count() - 1, im.joints()));
        return kInvalidNode;
    }
    if (!im.check_param(options.speed_param, ParamType::Float, true, "clip speed")) {
        return kInvalidNode;
    }
    if (options.sync_group != kNoSyncGroup && options.sync_group >= im.def->sync_groups.size()) {
        im.fail(std::format("AnimGraphBuilder: unknown sync group {}", options.sync_group));
        return kInvalidNode;
    }
    detail::ClipNodeDef cd;
    cd.clip = std::move(clip);
    cd.speed = options.speed;
    cd.wrap = options.wrap;
    cd.speed_param = options.speed_param;
    cd.sync_group = options.sync_group;
    cd.start_time = std::clamp(options.start_time, 0.0f, cd.clip->duration());
    im.def->clip_nodes.push_back(std::move(cd));
    return im.add_node(NodeType::Clip, static_cast<u32>(im.def->clip_nodes.size() - 1), {});
}

NodeId AnimGraphBuilder::add_blend1d(ParamId param, std::span<const Blend1DEntry> entries) {
    auto& im = *impl_;
    if (!im.check_param(param, ParamType::Float, false, "Blend1D")) {
        return kInvalidNode;
    }
    if (entries.empty()) {
        im.fail("AnimGraphBuilder: Blend1D needs at least one entry");
        return kInvalidNode;
    }
    std::vector<Blend1DEntry> sorted(entries.begin(), entries.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Blend1DEntry& a, const Blend1DEntry& b) { return a.threshold < b.threshold; });
    detail::Blend1DDef  bd;
    std::vector<NodeId> children;
    bd.param = param;
    for (const auto& e : sorted) {
        if (!im.node_ok(e.node) || !std::isfinite(e.threshold)) {
            im.fail("AnimGraphBuilder: Blend1D entry has an invalid node or threshold");
            return kInvalidNode;
        }
        children.push_back(e.node);
        bd.thresholds.push_back(e.threshold);
    }
    im.def->blend1d.push_back(std::move(bd));
    return im.add_node(NodeType::Blend1D, static_cast<u32>(im.def->blend1d.size() - 1),
                       std::move(children));
}

NodeId AnimGraphBuilder::add_blend2d(ParamId param_x, ParamId param_y,
                                     std::span<const Blend2DEntry> entries) {
    auto& im = *impl_;
    if (!im.check_param(param_x, ParamType::Float, false, "Blend2D x") ||
        !im.check_param(param_y, ParamType::Float, false, "Blend2D y")) {
        return kInvalidNode;
    }
    if (entries.empty()) {
        im.fail("AnimGraphBuilder: Blend2D needs at least one entry");
        return kInvalidNode;
    }
    detail::Blend2DDef  bd;
    std::vector<NodeId> children;
    bd.param_x = param_x;
    bd.param_y = param_y;
    for (usize i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (!im.node_ok(e.node)) {
            im.fail("AnimGraphBuilder: Blend2D entry has an invalid node");
            return kInvalidNode;
        }
        for (usize j = 0; j < i; ++j) {
            const Vec2 d = entries[j].position - e.position;
            if (d.x * d.x + d.y * d.y < 1.0e-10f) {
                im.fail("AnimGraphBuilder: Blend2D sample points must be distinct");
                return kInvalidNode;
            }
        }
        children.push_back(e.node);
        bd.points.push_back(e.position);
    }
    bd.triangulation = detail::triangulate_blend_points(bd.points);
    im.def->blend2d.push_back(std::move(bd));
    return im.add_node(NodeType::Blend2D, static_cast<u32>(im.def->blend2d.size() - 1),
                       std::move(children));
}

NodeId AnimGraphBuilder::add_additive(NodeId base, NodeId additive, AdditiveOptions options) {
    auto& im = *impl_;
    if (!im.node_ok(base) || !im.node_ok(additive)) {
        im.fail("AnimGraphBuilder: Additive node has an invalid child");
        return kInvalidNode;
    }
    if (!im.check_param(options.weight_param, ParamType::Float, true, "Additive weight")) {
        return kInvalidNode;
    }
    if (!options.mask.empty() && options.mask.joint_count() != im.joints()) {
        im.fail("AnimGraphBuilder: Additive mask size does not match the skeleton");
        return kInvalidNode;
    }
    detail::AdditiveDef ad;
    ad.weight = options.weight;
    ad.weight_param = options.weight_param;
    ad.mask = std::move(options.mask);
    if (im.def->skeleton) {
        const Skeleton& sk = *im.def->skeleton;
        ad.reference.assign(sk.bind_pose().begin(), sk.bind_pose().end());
        if (options.reference_clip) {
            if (options.reference_clip->required_joint_count() > sk.joint_count()) {
                im.fail("AnimGraphBuilder: additive reference clip does not fit the skeleton");
                return kInvalidNode;
            }
            options.reference_clip->sample(options.reference_time, ad.reference);
        }
    }
    im.def->additive.push_back(std::move(ad));
    return im.add_node(NodeType::Additive, static_cast<u32>(im.def->additive.size() - 1),
                       {base, additive});
}

NodeId AnimGraphBuilder::add_masked_layer(NodeId base, NodeId layer, BoneMask mask,
                                          const LayerOptions& options) {
    auto& im = *impl_;
    if (!im.node_ok(base) || !im.node_ok(layer)) {
        im.fail("AnimGraphBuilder: MaskedLayer node has an invalid child");
        return kInvalidNode;
    }
    if (!im.check_param(options.weight_param, ParamType::Float, true, "MaskedLayer weight")) {
        return kInvalidNode;
    }
    if (mask.joint_count() != im.joints()) {
        im.fail("AnimGraphBuilder: MaskedLayer mask size does not match the skeleton");
        return kInvalidNode;
    }
    detail::LayerDef ld;
    ld.weight = options.weight;
    ld.weight_param = options.weight_param;
    ld.mask = std::move(mask);
    im.def->layers.push_back(std::move(ld));
    return im.add_node(NodeType::MaskedLayer, static_cast<u32>(im.def->layers.size() - 1),
                       {base, layer});
}

NodeId AnimGraphBuilder::add_state_machine() {
    auto& im = *impl_;
    im.def->state_machines.emplace_back();
    return im.add_node(NodeType::StateMachine,
                       static_cast<u32>(im.def->state_machines.size() - 1), {});
}

StateId AnimGraphBuilder::add_state(NodeId state_machine, std::string name, NodeId node,
                                    const StateOptions& options) {
    auto& im = *impl_;
    detail::StateMachineDef* sm = im.machine(state_machine);
    if (sm == nullptr) {
        return kInvalidState;
    }
    if (!im.node_ok(node) || node == state_machine) {
        im.fail(std::format("AnimGraphBuilder: state '{}' has an invalid node", name));
        return kInvalidState;
    }
    for (const auto& s : sm->states) {
        if (s.name == name) {
            im.fail(std::format("AnimGraphBuilder: duplicate state name '{}'", name));
            return kInvalidState;
        }
    }
    sm->states.push_back({std::move(name), node, options.reset_on_enter});
    const auto id = static_cast<StateId>(sm->states.size() - 1);
    if (sm->entry == kInvalidState) {
        sm->entry = id;
    }
    return id;
}

void AnimGraphBuilder::set_entry_state(NodeId state_machine, StateId state) {
    auto&                    im = *impl_;
    detail::StateMachineDef* sm = im.machine(state_machine);
    if (sm == nullptr) {
        return;
    }
    if (state >= sm->states.size()) {
        im.fail("AnimGraphBuilder: set_entry_state with an invalid state");
        return;
    }
    sm->entry = state;
}

TransitionBuilder AnimGraphBuilder::add_transition(NodeId state_machine, StateId from, StateId to,
                                                   const TransitionOptions& options) {
    auto&                    im = *impl_;
    detail::StateMachineDef* sm = im.machine(state_machine);
    if (sm == nullptr) {
        return {this, state_machine, kInvalidU32};
    }
    const bool any = from == detail::kAnyState;
    if ((!any && from >= sm->states.size()) || to >= sm->states.size() ||
        !(options.duration >= 0.0f) || !(options.exit_time >= 0.0f)) {
        im.fail("AnimGraphBuilder: transition has an invalid state, duration or exit time");
        return {this, state_machine, kInvalidU32};
    }
    detail::TransitionDef td;
    td.from = from;
    td.to = to;
    td.options = options;
    sm->transitions.push_back(std::move(td));
    return {this, state_machine, static_cast<u32>(sm->transitions.size() - 1)};
}

TransitionBuilder AnimGraphBuilder::add_any_state_transition(NodeId state_machine, StateId to,
                                                             const TransitionOptions& options) {
    return add_transition(state_machine, detail::kAnyState, to, options);
}

void AnimGraphBuilder::add_condition(NodeId sm_node, u32 transition,
                                     const TransitionCondition& condition) {
    auto&                    im = *impl_;
    detail::StateMachineDef* sm = im.machine(sm_node);
    if (sm == nullptr || transition >= sm->transitions.size()) {
        im.fail("AnimGraphBuilder: condition added to an invalid transition");
        return;
    }
    ParamType type = ParamType::Float;
    switch (condition.op) {
    case ConditionOp::IsTrue:
    case ConditionOp::IsFalse: type = ParamType::Bool; break;
    case ConditionOp::Triggered: type = ParamType::Trigger; break;
    default: break;
    }
    if (!im.check_param(condition.param, type, false, "transition condition")) {
        return;
    }
    sm->transitions[transition].conditions.push_back(condition);
}

Result<std::shared_ptr<const AnimGraph>> AnimGraphBuilder::build(NodeId root) {
    using ResultT = std::shared_ptr<const AnimGraph>;
    if (!impl_) {
        return make_error<ResultT>(ErrorCode::InvalidArgument, "AnimGraphBuilder: already built");
    }
    Impl& im = *impl_;
    auto& def = *im.def;
    if (!im.error.empty()) {
        return make_error<ResultT>(ErrorCode::InvalidArgument, im.error);
    }
    if (!im.node_ok(root)) {
        return make_error<ResultT>(ErrorCode::InvalidArgument, "AnimGraphBuilder: invalid root");
    }

    // Finalize state machines: children = state nodes; transition lookup tables.
    for (auto& nd : def.nodes) {
        if (nd.type != NodeType::StateMachine) {
            continue;
        }
        auto& sm = def.state_machines[nd.payload];
        if (sm.states.empty()) {
            return make_error<ResultT>(ErrorCode::InvalidArgument,
                                       "AnimGraphBuilder: state machine without states");
        }
        nd.children.clear();
        for (const auto& s : sm.states) {
            nd.children.push_back(s.node);
        }
        sm.outgoing.assign(sm.states.size(), {});
        for (u32 t = 0; t < sm.transitions.size(); ++t) {
            const StateId from = sm.transitions[t].from;
            if (from == detail::kAnyState) {
                sm.any_state.push_back(t);
            } else {
                sm.outgoing[from].push_back(t);
            }
        }
    }

    // Tree check: each node has at most one parent, and the root none.
    std::vector<u32> parents(def.nodes.size(), 0);
    for (const auto& nd : def.nodes) {
        for (const NodeId c : nd.children) {
            if (++parents[c] > 1) {
                return make_error<ResultT>(
                    ErrorCode::InvalidArgument,
                    std::format("AnimGraphBuilder: node {} has more than one parent (the graph "
                                "must be a tree; create separate nodes)",
                                c));
            }
        }
    }
    if (parents[root] != 0) {
        return make_error<ResultT>(ErrorCode::InvalidArgument,
                                   "AnimGraphBuilder: the root node is a child of another node");
    }

    // Scratch pose levels: a node with children needs one temporary plus its deepest child's.
    std::function<u32(NodeId)> levels = [&](NodeId n) -> u32 {
        u32 deepest = 0;
        for (const NodeId c : def.nodes[n].children) {
            deepest = std::max(deepest, levels(c));
        }
        return def.nodes[n].children.empty() ? 0u : deepest + 1u;
    };
    def.pose_levels = levels(root);

    for (NodeId n = 0; n < def.nodes.size(); ++n) {
        if (def.nodes[n].type == NodeType::Clip) {
            const SyncGroupId g = def.clip_nodes[def.nodes[n].payload].sync_group;
            if (g != kNoSyncGroup) {
                def.sync_members[g].push_back(n);
            }
        }
    }
    def.root = root;

    std::shared_ptr<const AnimGraph> graph(new AnimGraph(std::move(im.def)));
    impl_.reset();
    return graph;
}

// ================================================================================================
// TransitionBuilder
// ================================================================================================
TransitionBuilder& TransitionBuilder::when(const TransitionCondition& condition) {
    if (builder_ != nullptr && builder_->impl_) {
        builder_->add_condition(sm_, transition_, condition);
    }
    return *this;
}
TransitionBuilder& TransitionBuilder::when_greater(ParamId p, f32 v) {
    return when({p, ConditionOp::Greater, v});
}
TransitionBuilder& TransitionBuilder::when_less(ParamId p, f32 v) {
    return when({p, ConditionOp::Less, v});
}
TransitionBuilder& TransitionBuilder::when_equal(ParamId p, f32 v) {
    return when({p, ConditionOp::Equal, v});
}
TransitionBuilder& TransitionBuilder::when_true(ParamId p) {
    return when({p, ConditionOp::IsTrue, 0.0f});
}
TransitionBuilder& TransitionBuilder::when_false(ParamId p) {
    return when({p, ConditionOp::IsFalse, 0.0f});
}
TransitionBuilder& TransitionBuilder::when_triggered(ParamId p) {
    return when({p, ConditionOp::Triggered, 0.0f});
}

} // namespace aether::animation
