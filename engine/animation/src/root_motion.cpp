// root_motion.cpp — root motion extraction / removal / application.
#include "aether/animation/root_motion.h"

#include "aether/animation/anim_math.h"
#include "playback.h"

namespace aether::animation {

namespace {

Transform root_model_at(const AnimationClip& clip, const Skeleton& skeleton, u32 joint, f32 t) {
    const Transform local = clip.sample_joint(joint, t, skeleton.bind_pose()[joint]);
    const i32       p = skeleton.parent(joint);
    return p < 0 ? local : compose(skeleton.bind_model_transform(static_cast<u32>(p)), local);
}

Transform to_transform(const RootMotionDelta& m) noexcept {
    return make_transform(m.translation, yaw_quat(m.yaw));
}

} // namespace

Quat RootMotionDelta::rotation() const noexcept { return yaw_quat(yaw); }

RootMotionDelta extract_motion(const Transform& model_root,
                               const RootMotionSettings& settings) noexcept {
    const Vec3&     p = model_root.position;
    RootMotionDelta m;
    m.translation = Vec3(settings.extract_horizontal ? p.x : 0.0f,
                         settings.extract_vertical ? p.y : 0.0f,
                         settings.extract_horizontal ? p.z : 0.0f);
    m.yaw = settings.extract_yaw ? twist_angle(model_root.rotation, Vec3(0.0f, 1.0f, 0.0f)) : 0.0f;
    return m;
}

RootMotionDelta combine(const RootMotionDelta& a, const RootMotionDelta& b) noexcept {
    RootMotionDelta r;
    r.translation = a.translation + rotate_yaw(a.yaw, b.translation);
    r.yaw = a.yaw + b.yaw;
    return r;
}

RootMotionDelta relative(const RootMotionDelta& from, const RootMotionDelta& to) noexcept {
    RootMotionDelta r;
    r.translation = rotate_yaw(-from.yaw, to.translation - from.translation);
    r.yaw = wrap_angle(to.yaw - from.yaw);
    return r;
}

RootMotionDelta extract_clip_root_motion(const AnimationClip& clip, const Skeleton& skeleton,
                                         const RootMotionSettings& settings, f64 from, f64 to,
                                         WrapMode mode) {
    RootMotionDelta total;
    const u32       joint = settings.joint;
    if (joint >= skeleton.joint_count() || !clip.animates(joint)) {
        return total;
    }
    detail::for_each_segment(from, to, clip.duration(), mode, [&](f32 a, f32 b, bool) {
        const RootMotionDelta ma = extract_motion(root_model_at(clip, skeleton, joint, a), settings);
        const RootMotionDelta mb = extract_motion(root_model_at(clip, skeleton, joint, b), settings);
        total = combine(total, relative(ma, mb));
    });
    return total;
}

void remove_root_motion(const Skeleton& skeleton, const RootMotionSettings& settings,
                        std::span<Transform> local) {
    const u32 joint = settings.joint;
    if (joint >= skeleton.joint_count() || joint >= local.size()) {
        return;
    }
    const i32       p = skeleton.parent(joint);
    const Transform parent = p < 0 ? Transform{} : skeleton.bind_model_transform(static_cast<u32>(p));
    const Transform model = p < 0 ? local[joint] : compose(parent, local[joint]);
    const Transform residual = compose(inverse(to_transform(extract_motion(model, settings))), model);
    local[joint] = p < 0 ? residual : compose(inverse(parent), residual);
}

void apply_root_motion(Transform& transform, const RootMotionDelta& delta) noexcept {
    transform.position += transform.rotation * (transform.scale * delta.translation);
    transform.rotation = normalize(transform.rotation * yaw_quat(delta.yaw));
}

} // namespace aether::animation
