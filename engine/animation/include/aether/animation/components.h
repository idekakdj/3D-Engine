// aether/animation/components.h — ECS components for skeletal animation.
//
// AnimatorComponent drives one skeleton: it owns an AnimGraphInstance, runs it each frame
// (tick), applies root-motion extraction and IK, and exposes the model-space pose and the
// skinning palette (model[j] * inverse_bind[j]). Pair it with a MeshRendererComponent whose mesh
// is skinned against the same skeleton; the gameplay layer copies palette() into
// RenderScene::joint_matrices (animation never touches the renderer).
//
// Runtime objects (Skeleton, AnimGraph) are shared_ptrs resolved by gameplay; the serializable
// settings (asset ids, playback, root motion) round-trip through the scene serializer via
// register_animation_codecs() (animation/serialization.h). Copying the component (editor
// duplicate / clone_entity) duplicates the full playback state.
//
// Thread-safety: not thread-safe; AnimationSubsystem ticks each animator on one worker at a time.
// Configure from the main thread outside AnimationSubsystem::update.
#pragma once

#include "aether/animation/anim_graph.h"
#include "aether/animation/ik.h"
#include "aether/animation/root_motion.h"
#include "aether/animation/skeleton.h"
#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace aether::animation {

struct TwoBoneIKChain {
    TwoBoneIKSettings settings;
    IKTargetSpace     space = IKTargetSpace::Model; // space of target / pole / target_rotation
    bool              enabled = true;
};

struct LookAtChain {
    LookAtSettings settings;
    IKTargetSpace  space = IKTargetSpace::Model;
    bool           enabled = true;
};

class AnimatorComponent {
public:
    AnimatorComponent() = default;
    explicit AnimatorComponent(std::shared_ptr<const Skeleton> skeleton,
                               std::shared_ptr<const AnimGraph> graph = nullptr);

    // ---- binding (main thread) ----
    // Sets the skeleton and resets outputs to its bind pose. Drops a graph built for another one.
    void set_skeleton(std::shared_ptr<const Skeleton> skeleton);
    // Creates a fresh instance of `graph`. Returns false (graph unchanged) if it was built for a
    // different skeleton than the current one (the skeleton is adopted when none is set).
    bool set_graph(std::shared_ptr<const AnimGraph> graph);
    [[nodiscard]] const std::shared_ptr<const Skeleton>& skeleton() const noexcept { return skeleton_; }
    [[nodiscard]] AnimGraphInstance&       graph_instance() noexcept { return instance_; }
    [[nodiscard]] const AnimGraphInstance& graph_instance() const noexcept { return instance_; }

    // Stable asset identities for serialization / the gameplay asset bridge (not interpreted here).
    AssetId skeleton_asset{};
    AssetId graph_asset{};

    // ---- parameters (forward to the graph instance) ----
    bool set_float(std::string_view name, f32 value) { return instance_.set_float(name, value); }
    bool set_bool(std::string_view name, bool value) { return instance_.set_bool(name, value); }
    bool set_trigger(std::string_view name) { return instance_.set_trigger(name); }
    void set_float(ParamId id, f32 value) { instance_.set_float(id, value); }
    void set_bool(ParamId id, bool value) { instance_.set_bool(id, value); }
    void set_trigger(ParamId id) { instance_.set_trigger(id); }

    // ---- playback ----
    void set_playback_rate(f32 rate) noexcept { playback_rate_ = rate; }
    [[nodiscard]] f32 playback_rate() const noexcept { return playback_rate_; }
    void set_paused(bool paused) noexcept { paused_ = paused; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; } // disabled = not ticked
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // ---- root motion ----
    void set_root_motion(RootMotionMode mode, const RootMotionSettings& settings = {}) noexcept {
        root_motion_mode_ = mode;
        root_motion_settings_ = settings;
    }
    [[nodiscard]] RootMotionMode root_motion_mode() const noexcept { return root_motion_mode_; }
    [[nodiscard]] const RootMotionSettings& root_motion_settings() const noexcept {
        return root_motion_settings_;
    }
    // Delta extracted by the last tick (zero in Ignore mode). Model/entity frame.
    [[nodiscard]] const RootMotionDelta& root_motion_delta() const noexcept { return root_motion_delta_; }

    // ---- IK (solved after the graph, before the palette; in insertion order) ----
    u32 add_two_bone_ik(const TwoBoneIKChain& chain);
    u32 add_look_at(const LookAtChain& chain);
    [[nodiscard]] TwoBoneIKChain& two_bone_ik(u32 index) { return two_bone_ik_[index]; }
    [[nodiscard]] LookAtChain&    look_at(u32 index) { return look_at_[index]; }
    [[nodiscard]] u32 two_bone_ik_count() const noexcept { return static_cast<u32>(two_bone_ik_.size()); }
    [[nodiscard]] u32 look_at_count() const noexcept { return static_cast<u32>(look_at_.size()); }
    void clear_ik() noexcept;

    // ---- per frame ----
    // update graph (dt * rate, 0 when paused) -> evaluate -> remove root motion -> IK -> model
    // pose -> palette. `entity_world` (the owner's world matrix) is needed only for world-space
    // IK targets. Allocation-free in steady state.
    void tick(f32 dt, const Mat4* entity_world = nullptr);

    // ---- outputs (valid after the first tick or a bind) ----
    [[nodiscard]] std::span<const Transform>       local_pose() const noexcept { return local_; }
    [[nodiscard]] std::span<const Mat4>            model_pose() const noexcept { return model_; }
    [[nodiscard]] std::span<const Mat4>            palette() const noexcept { return palette_; }
    [[nodiscard]] std::span<const AnimNotifyEvent> notifies() const noexcept { return instance_.notifies(); }

private:
    void reset_outputs();

    std::shared_ptr<const Skeleton> skeleton_;
    AnimGraphInstance               instance_;
    f32                             playback_rate_ = 1.0f;
    bool                            paused_ = false;
    bool                            enabled_ = true;
    RootMotionMode                  root_motion_mode_ = RootMotionMode::Ignore;
    RootMotionSettings              root_motion_settings_{};
    RootMotionDelta                 root_motion_delta_{};
    std::vector<TwoBoneIKChain>     two_bone_ik_;
    std::vector<LookAtChain>        look_at_;
    std::vector<Transform>          local_;
    std::vector<Mat4>               model_;
    std::vector<Mat4>               palette_;
};

} // namespace aether::animation
