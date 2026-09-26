// aether/animation/root_motion.h — root motion extraction (translation + yaw).
//
// A designated root joint's model-space transform R(t) is split into a planar "motion frame"
// M(t) (horizontal translation [+ optional vertical] + yaw about +Y) and a residual. Per update:
//   * the delta M(t0)^-1 * M(t1) is computed from each contributing clip's own curves, segment by
//     segment across loop/ping-pong wraps (so a loop wrap yields a small forward step, never a
//     jump back), and blended with the same weights as the poses;
//   * the motion frame is removed from the evaluated pose (the root keeps its residual: height,
//     pitch, roll), and the delta is reported for the gameplay layer to move the entity with
//     (entity_local = entity_local * delta), or applied to TransformComponent by the subsystem.
// Deltas are expressed in the entity's (model) frame at the start of the update. Ancestors of the
// root-motion joint are assumed unanimated (their bind pose is used). Per-segment yaw changes are
// assumed below 180 degrees.
//
// Thread-safety: pure functions; thread-safe on disjoint data.
#pragma once

#include "aether/animation/clip.h"
#include "aether/animation/skeleton.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <span>

namespace aether::animation {

enum class RootMotionMode : u8 {
    Ignore = 0,       // leave root motion in the pose (no extraction)
    Extract,          // remove it from the pose and report it (AnimatorComponent::root_motion_delta)
    ApplyToTransform, // Extract + the subsystem applies it to the entity's TransformComponent
};

struct RootMotionSettings {
    u32  joint = kInvalidJoint;     // designated root joint (kInvalidJoint = disabled)
    bool extract_horizontal = true; // model-space X/Z translation
    bool extract_vertical = false;  // model-space Y translation
    bool extract_yaw = true;        // rotation about +Y
};

// A planar rigid motion: translation, then yaw about +Y. Used both for motion frames and deltas.
struct RootMotionDelta {
    Vec3 translation{0.0f};
    f32  yaw = 0.0f; // radians (not wrapped: accumulated deltas may exceed pi)

    [[nodiscard]] Quat rotation() const noexcept;
};

// Motion frame of a model-space root transform under `settings`.
[[nodiscard]] RootMotionDelta extract_motion(const Transform& model_root,
                                             const RootMotionSettings& settings) noexcept;
// a then b (b expressed in a's frame).
[[nodiscard]] RootMotionDelta combine(const RootMotionDelta& a, const RootMotionDelta& b) noexcept;
// from^-1 * to: the motion taking frame `from` to frame `to`, in `from`'s frame.
[[nodiscard]] RootMotionDelta relative(const RootMotionDelta& from,
                                       const RootMotionDelta& to) noexcept;

// Root motion of `clip` while its unwrapped playback time moves from -> to under `mode`.
[[nodiscard]] RootMotionDelta extract_clip_root_motion(const AnimationClip& clip,
                                                       const Skeleton& skeleton,
                                                       const RootMotionSettings& settings,
                                                       f64 from, f64 to, WrapMode mode);

// Removes the motion frame from the root joint of a local-space pose (in place).
void remove_root_motion(const Skeleton& skeleton, const RootMotionSettings& settings,
                        std::span<Transform> local);

// transform = transform * delta (moves an entity by a root-motion delta in its own frame).
void apply_root_motion(Transform& transform, const RootMotionDelta& delta) noexcept;

} // namespace aether::animation
