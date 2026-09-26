// aether/animation/anim_graph.h — runtime animation graph: blend trees + state machines.
//
// AnimGraph is an immutable, shareable definition (the "compiled AnimBlueprint") built through
// AnimGraphBuilder against one Skeleton. Each animated character owns an AnimGraphInstance with
// all mutable state: parameter values, clip times, state-machine state, sync-group phases and
// pre-allocated pose scratch buffers. Per frame:
//     instance.set_float(speed, 3.2f);      // parameters (gameplay / scripts)
//     instance.update(dt, &root_motion);    // time, weights, transitions, notifies, root motion
//     instance.evaluate(pose);              // sample + blend into a local-space pose
// Neither update nor evaluate allocates after construction (the notify list grows only the
// first time a frame emits more events than ever before).
//
// Nodes
//   Clip         plays one clip (speed, optional speed parameter, wrap mode, sync group).
//   Blend1D      N children at ascending thresholds of a float parameter (2 active at most).
//   Blend2D      children at 2D sample points; barycentric weights inside a Delaunay
//                triangulation, projection onto the hull outside (3 active at most).
//   Additive     base + weight * (additive child - reference pose) [optional bone mask].
//   MaskedLayer  base overridden by a layer child with per-joint mask * weight.
//   StateMachine states (each holds a node), transitions with parameter conditions, crossfade
//                duration + curve, exit time, any-state transitions and interruption rules.
// The graph must be a tree: every node has at most one parent.
//
// State machine semantics (Unity/Unreal-like)
//   * Transitions are checked at the start of each update against the state's progress from the
//     previous update (one-frame latency). Any-state transitions are checked first, then the
//     current state's outgoing transitions, in declaration order; the first passing one fires.
//   * Conditions are ANDed. Trigger parameters stay set until a firing transition that tests
//     them consumes them (or reset_trigger()).
//   * Exit time is in normalized state time (1.0 = one full length of the state's dominant clip
//     since entry). For looping states an exit time < 1 is satisfied each time the cycle crosses
//     it; otherwise (>= 1, or non-looping) once the state's time has reached it.
//   * A crossfade blends the live source into the destination. When a transition is interrupted
//     (per its InterruptionSource) or a state transitions to itself, the source is the frozen
//     pose from the previous frame (pose snapshot), as in Unity.
//
// Sync groups phase-match clips (e.g. walk/run cycles) by normalized time: members of a group
// share one phase that advances at the weight-averaged cycle rate of the relevant members, so
// footfalls stay aligned while blending and across state transitions.
//
// Thread-safety: AnimGraph is immutable (safe to share across threads). An AnimGraphInstance must
// be used by one thread at a time. The builder is single-threaded, setup-time only.
#pragma once

#include "aether/animation/clip.h"
#include "aether/animation/pose.h"
#include "aether/animation/root_motion.h"
#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace aether::animation {

class Skeleton;

using NodeId = u32;
using ParamId = u32;
using StateId = u32;
using SyncGroupId = u32;

inline constexpr NodeId      kInvalidNode = kInvalidU32;
inline constexpr ParamId     kInvalidParam = kInvalidU32;
inline constexpr StateId     kInvalidState = kInvalidU32;
inline constexpr SyncGroupId kNoSyncGroup = kInvalidU32;

enum class ParamType : u8 { Float = 0, Bool, Trigger };

enum class ConditionOp : u8 {
    Greater = 0, // float >  value
    Less,        // float <  value
    Equal,       // |float - value| <= 1e-5
    NotEqual,
    IsTrue,      // bool
    IsFalse,     // bool
    Triggered,   // trigger (consumed when the transition fires)
};

struct TransitionCondition {
    ParamId     param = kInvalidParam;
    ConditionOp op = ConditionOp::IsTrue;
    f32         value = 0.0f;
};

// Which transitions may interrupt an in-progress transition (any-state transitions always may,
// unless this is None).
enum class InterruptionSource : u8 {
    None = 0,              // runs to completion
    Source,                // transitions leaving the source state
    Destination,           // transitions leaving the destination state
    SourceThenDestination,
    DestinationThenSource,
};

enum class BlendCurve : u8 { Linear = 0, SmoothStep };

struct ClipNodeOptions {
    f32         speed = 1.0f;
    WrapMode    wrap = WrapMode::Loop;
    ParamId     speed_param = kInvalidParam; // optional float multiplier on speed
    SyncGroupId sync_group = kNoSyncGroup;
    f32         start_time = 0.0f;           // seconds; where the clip (re)starts
};

struct Blend1DEntry {
    NodeId node = kInvalidNode;
    f32    threshold = 0.0f;
};

struct Blend2DEntry {
    NodeId node = kInvalidNode;
    Vec2   position{0.0f};
};

struct AdditiveOptions {
    f32     weight = 1.0f;
    ParamId weight_param = kInvalidParam; // optional float multiplier, result clamped to [0, 1]
    // Reference pose the additive child is measured against: the skeleton bind pose, or a frame
    // of `reference_clip` (e.g. the first frame of the additive clip itself).
    std::shared_ptr<const AnimationClip> reference_clip;
    f32                                  reference_time = 0.0f;
    BoneMask                             mask; // empty = all joints
};

struct LayerOptions {
    f32     weight = 1.0f;
    ParamId weight_param = kInvalidParam; // optional float multiplier, result clamped to [0, 1]
};

struct StateOptions {
    bool reset_on_enter = true; // restart the state's clips when entered
};

struct TransitionOptions {
    f32                duration = 0.2f; // crossfade seconds (0 = instant)
    bool               has_exit_time = false;
    f32                exit_time = 1.0f; // normalized state time
    InterruptionSource interruption = InterruptionSource::None;
    BlendCurve         curve = BlendCurve::Linear;
    bool               can_transition_to_self = false; // any-state transitions only
};

// A notify crossed during the last update.
struct AnimNotifyEvent {
    std::string_view     name;              // points into the clip (kept alive by the graph)
    f32                  time = 0.0f;       // clip-local time of the notify
    f32                  weight = 0.0f;     // blend weight of the emitting clip node
    NodeId               node = kInvalidNode;
    const AnimationClip* clip = nullptr;
};

namespace detail {
struct AnimGraphDef; // private definition (src/anim_graph_internal.h)
}

class AnimGraph {
public:
    ~AnimGraph();
    AnimGraph(const AnimGraph&) = delete;
    AnimGraph& operator=(const AnimGraph&) = delete;

    // Convenience: a graph that just plays one clip.
    [[nodiscard]] static Result<std::shared_ptr<const AnimGraph>> make_single_clip(
        std::shared_ptr<const Skeleton> skeleton, std::shared_ptr<const AnimationClip> clip,
        WrapMode wrap = WrapMode::Loop, f32 speed = 1.0f);

    [[nodiscard]] const Skeleton& skeleton() const noexcept;
    [[nodiscard]] const std::shared_ptr<const Skeleton>& skeleton_ptr() const noexcept;
    [[nodiscard]] NodeId root() const noexcept;
    [[nodiscard]] u32 node_count() const noexcept;

    [[nodiscard]] u32 parameter_count() const noexcept;
    [[nodiscard]] ParamId find_parameter(std::string_view name) const noexcept;
    [[nodiscard]] const std::string& parameter_name(ParamId id) const;
    [[nodiscard]] ParamType parameter_type(ParamId id) const;

    [[nodiscard]] SyncGroupId find_sync_group(std::string_view name) const noexcept;
    // kInvalidState if `state_machine` is not a state machine node or the name is unknown.
    [[nodiscard]] StateId find_state(NodeId state_machine, std::string_view name) const noexcept;
    [[nodiscard]] u32 state_count(NodeId state_machine) const noexcept;

    // Implementation detail (opaque outside the module).
    [[nodiscard]] const detail::AnimGraphDef& definition() const noexcept { return *def_; }

private:
    friend class AnimGraphBuilder;
    explicit AnimGraph(std::unique_ptr<detail::AnimGraphDef> def);
    std::unique_ptr<detail::AnimGraphDef> def_;
};

class AnimGraphBuilder;

// Fluent handle returned by add_transition: .when_greater(speed, 0.1f).when_true(grounded)
class TransitionBuilder {
public:
    TransitionBuilder& when(const TransitionCondition& condition);
    TransitionBuilder& when_greater(ParamId param, f32 value);
    TransitionBuilder& when_less(ParamId param, f32 value);
    TransitionBuilder& when_equal(ParamId param, f32 value);
    TransitionBuilder& when_true(ParamId param);
    TransitionBuilder& when_false(ParamId param);
    TransitionBuilder& when_triggered(ParamId param);
    [[nodiscard]] u32 index() const noexcept { return transition_; }

private:
    friend class AnimGraphBuilder;
    TransitionBuilder(AnimGraphBuilder* builder, NodeId sm, u32 transition)
        : builder_(builder), sm_(sm), transition_(transition) {}
    AnimGraphBuilder* builder_ = nullptr;
    NodeId            sm_ = kInvalidNode;
    u32               transition_ = kInvalidU32;
};

// Builds an AnimGraph. Invalid arguments do not abort: the first error is recorded, the call
// returns an invalid id, and build() reports the error. build() consumes the builder.
class AnimGraphBuilder {
public:
    explicit AnimGraphBuilder(std::shared_ptr<const Skeleton> skeleton);
    ~AnimGraphBuilder();
    AnimGraphBuilder(AnimGraphBuilder&&) noexcept;
    AnimGraphBuilder& operator=(AnimGraphBuilder&&) noexcept;
    AnimGraphBuilder(const AnimGraphBuilder&) = delete;
    AnimGraphBuilder& operator=(const AnimGraphBuilder&) = delete;

    // ---- parameters (names unique) ----
    ParamId add_float(std::string name, f32 default_value = 0.0f);
    ParamId add_bool(std::string name, bool default_value = false);
    ParamId add_trigger(std::string name);
    SyncGroupId add_sync_group(std::string name);
    // Notifies are emitted only by clip nodes whose weight exceeds this (default 0).
    void set_notify_weight_threshold(f32 threshold);

    // ---- nodes ----
    NodeId add_clip(std::shared_ptr<const AnimationClip> clip, const ClipNodeOptions& options = {});
    NodeId add_blend1d(ParamId param, std::span<const Blend1DEntry> entries);
    NodeId add_blend1d(ParamId param, std::initializer_list<Blend1DEntry> entries) {
        return add_blend1d(param, std::span<const Blend1DEntry>(entries.begin(), entries.size()));
    }
    NodeId add_blend2d(ParamId param_x, ParamId param_y, std::span<const Blend2DEntry> entries);
    NodeId add_blend2d(ParamId param_x, ParamId param_y, std::initializer_list<Blend2DEntry> entries) {
        return add_blend2d(param_x, param_y,
                           std::span<const Blend2DEntry>(entries.begin(), entries.size()));
    }
    NodeId add_additive(NodeId base, NodeId additive, AdditiveOptions options = {});
    NodeId add_masked_layer(NodeId base, NodeId layer, BoneMask mask,
                            const LayerOptions& options = {});

    // ---- state machines ----
    NodeId  add_state_machine();
    // The first state added becomes the entry state unless set_entry_state is called.
    StateId add_state(NodeId state_machine, std::string name, NodeId node,
                      const StateOptions& options = {});
    void    set_entry_state(NodeId state_machine, StateId state);
    TransitionBuilder add_transition(NodeId state_machine, StateId from, StateId to,
                                     const TransitionOptions& options = {});
    TransitionBuilder add_any_state_transition(NodeId state_machine, StateId to,
                                               const TransitionOptions& options = {});

    // Validates (tree shape, parameter types, non-empty state machines, ...) and finalizes.
    [[nodiscard]] Result<std::shared_ptr<const AnimGraph>> build(NodeId root);

private:
    friend class TransitionBuilder;
    void add_condition(NodeId sm, u32 transition, const TransitionCondition& condition);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Per-character runtime state of an AnimGraph. Copying duplicates the full playback state.
class AnimGraphInstance {
public:
    AnimGraphInstance();
    explicit AnimGraphInstance(std::shared_ptr<const AnimGraph> graph);
    ~AnimGraphInstance();
    AnimGraphInstance(const AnimGraphInstance& other);
    AnimGraphInstance& operator=(const AnimGraphInstance& other);
    AnimGraphInstance(AnimGraphInstance&&) noexcept;
    AnimGraphInstance& operator=(AnimGraphInstance&&) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }
    [[nodiscard]] const std::shared_ptr<const AnimGraph>& graph() const noexcept;

    // ---- parameters: by id (fast) or by name (hash lookup; false if unknown / wrong type) ----
    void set_float(ParamId id, f32 value);
    void set_bool(ParamId id, bool value);
    void set_trigger(ParamId id);
    void reset_trigger(ParamId id);
    [[nodiscard]] f32  get_float(ParamId id) const;
    [[nodiscard]] bool get_bool(ParamId id) const;
    [[nodiscard]] bool is_trigger_set(ParamId id) const;
    bool set_float(std::string_view name, f32 value);
    bool set_bool(std::string_view name, bool value);
    bool set_trigger(std::string_view name);

    // Restarts playback (clip times, state machines, sync phases); keeps parameter values.
    void reset();
    // Restores every parameter to its default value.
    void reset_parameters();

    // Advances the graph by `dt` seconds. Root motion is computed when `root_motion` is non-null
    // and names a valid joint (it must stay valid for the duration of the call only).
    void update(f32 dt, const RootMotionSettings* root_motion = nullptr);
    // Writes the current local-space pose (out.size() >= joint count). Calls update(0) first if
    // the instance was never updated.
    void evaluate(std::span<Transform> out_local);

    // Results of the last update.
    [[nodiscard]] std::span<const AnimNotifyEvent> notifies() const noexcept;
    [[nodiscard]] const RootMotionDelta& root_motion() const noexcept;

    // ---- introspection (debug UI, tests) ----
    [[nodiscard]] StateId current_state(NodeId state_machine) const;
    [[nodiscard]] bool    in_transition(NodeId state_machine) const;
    [[nodiscard]] StateId transition_source(NodeId state_machine) const; // kInvalidState if frozen/none
    [[nodiscard]] f32     transition_alpha(NodeId state_machine) const;  // 1 when not transitioning
    [[nodiscard]] f64     state_normalized_time(NodeId state_machine, StateId state) const;
    [[nodiscard]] f32     node_weight(NodeId node) const;    // weight in the last update
    [[nodiscard]] f32     clip_time(NodeId clip_node) const; // clip-local sample time
    [[nodiscard]] f64     sync_phase(SyncGroupId group) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::animation
