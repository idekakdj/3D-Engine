// anim_test_utils.h — shared helpers for the animation tests.
#pragma once

#include "aether/animation/anim_math.h"
#include "aether/animation/clip.h"
#include "aether/animation/skeleton.h"
#include "aether/assets/asset_types.h"

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

namespace anim_test {

using namespace aether;
using namespace aether::animation;
using assets::AnimPath;
using assets::Interpolation;

inline bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::abs(a - b) <= eps; }
inline bool near(const Vec3& a, const Vec3& b, f32 eps = 1.0e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
// Same rotation (q and -q are equivalent).
inline bool near(const Quat& a, const Quat& b, f32 eps = 1.0e-4f) {
    const f32 s = dot(a, b) < 0.0f ? -1.0f : 1.0f;
    return near(a.w, s * b.w, eps) && near(a.x, s * b.x, eps) && near(a.y, s * b.y, eps) &&
           near(a.z, s * b.z, eps);
}
inline bool near(const Mat4& a, const Mat4& b, f32 eps = 1.0e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!near(a[c][r], b[c][r], eps)) {
                return false;
            }
        }
    }
    return true;
}

inline Quat rot_deg(f32 degrees, const Vec3& axis) { return angle_axis(degrees * kDeg2Rad, axis); }
inline const Vec3 kX{1.0f, 0.0f, 0.0f};
inline const Vec3 kY{0.0f, 1.0f, 0.0f};
inline const Vec3 kZ{0.0f, 0.0f, 1.0f};

// n joints in a chain along +Y, `bone` apart, root at the origin.
inline assets::SkeletonData chain_data(u32 n, f32 bone = 1.0f) {
    assets::SkeletonData d;
    for (u32 i = 0; i < n; ++i) {
        d.joint_names.push_back("j" + std::to_string(i));
        d.parents.push_back(static_cast<i32>(i) - 1);
        Transform t;
        t.position = i == 0 ? Vec3(0.0f) : Vec3(0.0f, bone, 0.0f);
        d.bind_local.push_back(t);
    }
    return d;
}

inline std::shared_ptr<const Skeleton> make_skeleton(const assets::SkeletonData& d) {
    auto r = Skeleton::create(d);
    REQUIRE(r.has_value());
    return std::make_shared<const Skeleton>(std::move(r.value()));
}

inline assets::AnimationChannel channel(u32 joint, AnimPath path, Interpolation interp,
                                        std::initializer_list<f32> times,
                                        std::initializer_list<Vec4> values) {
    assets::AnimationChannel c;
    c.joint = joint;
    c.path = path;
    c.interpolation = interp;
    c.times = times;
    c.values = values;
    return c;
}

inline AnimationClip make_clip_value(const assets::AnimationClipData& d, u32 joints) {
    auto r = AnimationClip::create(d, joints);
    REQUIRE(r.has_value());
    return std::move(r.value());
}

// Constant clip: each (joint, translation) pair held for `duration` seconds.
inline std::shared_ptr<const AnimationClip> pose_clip(
    u32 joints, f32 duration, std::initializer_list<std::pair<u32, Vec3>> translations) {
    assets::AnimationClipData d;
    d.name = "pose";
    d.duration = duration;
    for (const auto& [joint, t] : translations) {
        d.channels.push_back(
            channel(joint, AnimPath::Translation, Interpolation::Linear, {0.0f}, {Vec4(t, 0.0f)}));
    }
    return std::make_shared<const AnimationClip>(make_clip_value(d, joints));
}

// Joint 0 translated along x: constant x.
inline std::shared_ptr<const AnimationClip> x_clip(u32 joints, f32 x, f32 duration = 1.0f) {
    return pose_clip(joints, duration, {{0u, Vec3(x, 0.0f, 0.0f)}});
}

} // namespace anim_test
