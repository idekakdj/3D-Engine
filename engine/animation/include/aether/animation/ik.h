// aether/animation/ik.h — analytic inverse kinematics on local-space poses.
//
// Solvers read the model-space chain from a local pose, solve in model space and write the
// resulting LOCAL rotations back (positions untouched), blended with the FK pose by `weight`.
// Model space = skeleton root space (convert world targets with the entity's inverse world
// matrix first; AnimatorComponent does this for IKTargetSpace::World).
//
// Thread-safety: pure functions; thread-safe on disjoint poses.
#pragma once

#include "aether/animation/anim_math.h"
#include "aether/animation/skeleton.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <span>

namespace aether::animation {

enum class IKTargetSpace : u8 { Model = 0, World };

// Two-bone IK (thigh/calf/foot, upperarm/forearm/hand). `mid` must descend from `root` and `tip`
// from `mid` (usually direct children; intermediate joints are carried rigidly).
struct TwoBoneIKSettings {
    u32  root = kInvalidJoint;
    u32  mid = kInvalidJoint;
    u32  tip = kInvalidJoint;
    Vec3 target{0.0f};     // desired tip position (model space)
    Vec3 pole{0.0f};       // point the mid joint bends toward (model space), if use_pole
    bool use_pole = false; // otherwise the current bend plane is kept
    f32  weight = 1.0f;    // 0 = FK pose, 1 = full IK
    // Soft limit: fraction of the chain length over which the approach to full extension is
    // eased (0 = hard, typical 0.02-0.1). Prevents the knee "snap" near full extension.
    f32  softness = 0.0f;
    bool use_target_rotation = false; // also set the tip's model-space rotation
    Quat target_rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Returns false (pose unchanged) for an invalid chain or a degenerate configuration (zero-length
// bone, target at the root). Unreachable targets fully extend the chain toward the target.
bool solve_two_bone_ik(const Skeleton& skeleton, std::span<Transform> local,
                       const TwoBoneIKSettings& settings);

// Single-joint look-at: rotates `joint` so its local `forward_axis` points at `target`.
struct LookAtSettings {
    u32  joint = kInvalidJoint;
    Vec3 target{0.0f};                     // model space
    Vec3 forward_axis{0.0f, 0.0f, 1.0f};   // joint-local unit axis that should face the target
    f32  weight = 1.0f;
    f32  max_angle = kPi;                  // clamp on the correction (radians)
};

bool solve_look_at(const Skeleton& skeleton, std::span<Transform> local,
                   const LookAtSettings& settings);

} // namespace aether::animation
