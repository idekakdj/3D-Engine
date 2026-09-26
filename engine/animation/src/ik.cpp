// ik.cpp — analytic two-bone IK (law of cosines + swing + pole twist) and look-at.
//
// Two-bone solve, in model space with a = root, b = mid, c = tip, t = target:
//   1. bend:  rotate the mid joint about the chain plane normal so the interior angle at b
//             gives |c - a| == d (law of cosines), d = clamped/softened |t - a|;
//   2. swing: rotate the root so a->c points along a->t (shortest arc);
//   3. pole:  twist the root about a->t so b lies in the plane of a, t and the pole.
// With R = pole * swing and S = bend (both model-space), the new local rotations are
//   root: inv(parent_model_rot) * R * root_model_rot
//   mid:  inv(mid_parent_model_rot) * S * mid_model_rot    (R cancels: it also rotates the parent)
//   tip:  inv(R * S * tip_parent_model_rot) * target_rotation   (optional)
#include "aether/animation/ik.h"

#include "aether/animation/pose.h"
#include "aether/core/error.h"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

Transform parent_model(const Skeleton& skeleton, std::span<const Transform> local, u32 joint) {
    const i32 p = skeleton.parent(joint);
    return p < 0 ? Transform{} : model_transform(skeleton, local, static_cast<u32>(p));
}

// Any unit vector perpendicular to unit `v`.
Vec3 any_perpendicular(const Vec3& v) {
    const Vec3 axis = std::abs(v.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    return normalize_or(cross(v, axis), Vec3(0.0f, 0.0f, 1.0f));
}

} // namespace

bool solve_two_bone_ik(const Skeleton& skeleton, std::span<Transform> local,
                       const TwoBoneIKSettings& s) {
    const u32 n = skeleton.joint_count();
    if (s.root >= n || s.mid >= n || s.tip >= n || local.size() < n ||
        !skeleton.is_ancestor(s.root, s.mid) || !skeleton.is_ancestor(s.mid, s.tip)) {
        return false;
    }
    if (s.weight <= 0.0f) {
        return true;
    }
    const f32 weight = std::min(s.weight, 1.0f);

    const Transform root_parent = parent_model(skeleton, local, s.root);
    const Transform mid_parent = parent_model(skeleton, local, s.mid);
    const Transform tip_parent = parent_model(skeleton, local, s.tip);
    const Transform root_m = compose(root_parent, local[s.root]);
    const Transform mid_m = compose(mid_parent, local[s.mid]);
    const Transform tip_m = compose(tip_parent, local[s.tip]);

    const Vec3 a = root_m.position;
    Vec3       b = mid_m.position;
    Vec3       c = tip_m.position;
    const f32  l1 = length(b - a);
    const f32  l2 = length(c - b);
    const Vec3 to_target = s.target - a;
    f32        d = length(to_target);
    if (l1 < kAnimEpsilon || l2 < kAnimEpsilon || d < kAnimEpsilon) {
        return false;
    }
    const Vec3 dir = to_target / d;

    // Soft limit (exponential approach to full extension), then clamp to the reachable range.
    const f32 chain = l1 + l2;
    const f32 softness = std::clamp(s.softness, 0.0f, 0.99f);
    if (softness > 0.0f) {
        const f32 soft_start = chain * (1.0f - softness);
        const f32 soft_len = chain - soft_start;
        if (d > soft_start) {
            d = soft_start + soft_len * (1.0f - std::exp(-(d - soft_start) / soft_len));
        }
    }
    const f32 margin = chain * 1.0e-5f;
    d = std::clamp(d, std::abs(l1 - l2) + margin, chain - margin);

    // 1) bend at the mid joint.
    const f32 cos_now = dot(normalize_or(a - b, Vec3(0.0f)), normalize_or(c - b, Vec3(0.0f)));
    const f32 cos_want = (l1 * l1 + l2 * l2 - d * d) / (2.0f * l1 * l2);
    const f32 angle_now = std::acos(std::clamp(cos_now, -1.0f, 1.0f));
    const f32 angle_want = std::acos(std::clamp(cos_want, -1.0f, 1.0f));
    Vec3      axis = cross(c - a, b - a); // positive rotation about it opens the joint
    if (length_squared(axis) < 1.0e-10f * l1 * l1 * l2 * l2) {
        // Straight chain: bend so the knee ends up toward the pole (or any consistent side).
        axis = s.use_pole ? cross(c - a, s.pole - a) : Vec3(0.0f);
        if (length_squared(axis) < 1.0e-12f) {
            axis = any_perpendicular(normalize_or(c - a, Vec3(0.0f, 1.0f, 0.0f)));
        }
    }
    axis = normalize_or(axis, Vec3(1.0f, 0.0f, 0.0f));
    const Quat bend = angle_axis(angle_want - angle_now, axis);
    c = b + bend * (c - b);

    // 2) swing the root so the chain aims at the target.
    const Quat swing = rotation_between(normalize_or(c - a, dir), dir);
    b = a + swing * (b - a);

    // 3) pole twist about the target axis.
    Quat pole_twist = quat_identity();
    if (s.use_pole) {
        const Vec3 bp = (b - a) - dir * dot(b - a, dir);
        const Vec3 pp = (s.pole - a) - dir * dot(s.pole - a, dir);
        if (length_squared(bp) > 1.0e-10f && length_squared(pp) > 1.0e-10f) {
            const f32 angle = std::atan2(dot(cross(bp, pp), dir), dot(bp, pp));
            pole_twist = angle_axis(angle, dir);
        }
    }
    const Quat r = pole_twist * swing;

    const Quat root_local = normalize(conjugate(root_parent.rotation) * r * root_m.rotation);
    const Quat mid_local = normalize(conjugate(mid_parent.rotation) * bend * mid_m.rotation);
    local[s.root].rotation = nlerp(local[s.root].rotation, root_local, weight);
    local[s.mid].rotation = nlerp(local[s.mid].rotation, mid_local, weight);
    if (s.use_target_rotation) {
        const Quat tip_parent_rot = r * bend * tip_parent.rotation;
        const Quat tip_local = normalize(conjugate(tip_parent_rot) * s.target_rotation);
        local[s.tip].rotation = nlerp(local[s.tip].rotation, tip_local, weight);
    }
    return true;
}

bool solve_look_at(const Skeleton& skeleton, std::span<Transform> local, const LookAtSettings& s) {
    const u32 n = skeleton.joint_count();
    if (s.joint >= n || local.size() < n) {
        return false;
    }
    if (s.weight <= 0.0f) {
        return true;
    }
    const Transform parent = parent_model(skeleton, local, s.joint);
    const Transform joint_m = compose(parent, local[s.joint]);
    const Vec3      to_target = s.target - joint_m.position;
    if (length_squared(to_target) < 1.0e-12f) {
        return false;
    }
    const Vec3 forward = normalize_or(joint_m.rotation * s.forward_axis, Vec3(0.0f, 0.0f, 1.0f));
    Quat       delta = rotation_between(forward, normalize_or(to_target, forward));
    const f32  angle = quat_angle(delta);
    if (s.max_angle < kPi && angle > s.max_angle && angle > kAnimEpsilon) {
        delta = slerp(quat_identity(), delta, s.max_angle / angle);
    }
    const Quat solved = normalize(conjugate(parent.rotation) * delta * joint_m.rotation);
    local[s.joint].rotation = nlerp(local[s.joint].rotation, solved, std::min(s.weight, 1.0f));
    return true;
}

} // namespace aether::animation
