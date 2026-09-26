// pose.cpp — pose hierarchy, blending, additive and mask operations.
#include "aether/animation/pose.h"

#include "aether/animation/anim_math.h"
#include "aether/animation/skeleton.h"
#include "aether/core/error.h"
#include "aether/core/log.h"

#include <algorithm>

namespace aether::animation {

// ---- BoneMask / Pose ----------------------------------------------------------------------------
BoneMask BoneMask::from_branch(const Skeleton& skeleton, u32 joint, f32 weight) {
    BoneMask mask(skeleton.joint_count(), 0.0f);
    mask.set_branch(skeleton, joint, weight);
    return mask;
}

BoneMask BoneMask::from_branch(const Skeleton& skeleton, std::string_view joint, f32 weight) {
    const u32 index = skeleton.find_joint(joint);
    if (index == kInvalidJoint) {
        AE_LOG_WARN("Animation", "BoneMask: unknown joint '{}'; mask is empty", joint);
        return BoneMask(skeleton.joint_count(), 0.0f);
    }
    return from_branch(skeleton, index, weight);
}

void BoneMask::set_branch(const Skeleton& skeleton, u32 joint, f32 weight) {
    const u32 n = skeleton.joint_count();
    if (joint >= n) {
        return;
    }
    if (weights_.size() < n) {
        weights_.resize(n, 0.0f);
    }
    // Topological order: a joint is in the branch iff its parent is (single forward pass).
    std::vector<u8> in_branch(n, 0);
    in_branch[joint] = 1;
    weights_[joint] = weight;
    for (u32 i = joint + 1; i < n; ++i) {
        const i32 p = skeleton.parent(i);
        if (p >= 0 && in_branch[static_cast<u32>(p)] != 0) {
            in_branch[i] = 1;
            weights_[i] = weight;
        }
    }
}

Pose::Pose(const Skeleton& skeleton) { set_bind(skeleton); }

void Pose::set_bind(const Skeleton& skeleton) {
    const auto bind = skeleton.bind_pose();
    local_.assign(bind.begin(), bind.end());
}

void Pose::set_identity() { std::fill(local_.begin(), local_.end(), Transform{}); }

// ---- hierarchy ----------------------------------------------------------------------------------
void local_to_model(const Skeleton& skeleton, std::span<const Transform> local,
                    std::span<Mat4> out_model) {
    const u32 n = skeleton.joint_count();
    AE_ASSERT(local.size() >= n && out_model.size() >= n);
    const i32* parents = skeleton.parents().data();
    for (u32 i = 0; i < n; ++i) {
        const Mat4 m = to_mat4(local[i]);
        out_model[i] = parents[i] < 0 ? m : out_model[static_cast<u32>(parents[i])] * m;
    }
}

Transform model_transform(const Skeleton& skeleton, std::span<const Transform> local, u32 joint) {
    AE_ASSERT(joint < skeleton.joint_count() && joint < local.size());
    Transform result = local[joint];
    for (i32 p = skeleton.parent(joint); p >= 0; p = skeleton.parent(static_cast<u32>(p))) {
        result = compose(local[static_cast<u32>(p)], result);
    }
    return result;
}

void compute_skinning_palette(std::span<const Mat4> model, std::span<const Mat4> inverse_bind,
                              std::span<Mat4> out_palette) {
    const usize n = std::min({model.size(), inverse_bind.size(), out_palette.size()});
    for (usize i = 0; i < n; ++i) {
        out_palette[i] = model[i] * inverse_bind[i];
    }
}

// ---- blending -----------------------------------------------------------------------------------
void blend_poses(std::span<const Transform> a, std::span<const Transform> b, f32 weight,
                 std::span<Transform> out) {
    const usize n = out.size();
    AE_ASSERT(a.size() >= n && b.size() >= n);
    if (weight <= 0.0f) {
        if (a.data() != out.data()) {
            std::copy_n(a.begin(), n, out.begin());
        }
        return;
    }
    if (weight >= 1.0f) {
        if (b.data() != out.data()) {
            std::copy_n(b.begin(), n, out.begin());
        }
        return;
    }
    for (usize i = 0; i < n; ++i) {
        out[i] = lerp(a[i], b[i], weight);
    }
}

void blend_poses_masked(std::span<const Transform> a, std::span<const Transform> b, f32 weight,
                        const BoneMask& mask, std::span<Transform> out) {
    const usize n = out.size();
    AE_ASSERT(a.size() >= n && b.size() >= n);
    const auto weights = mask.weights();
    for (usize i = 0; i < n; ++i) {
        const f32 w = i < weights.size() ? weight * weights[i] : 0.0f;
        if (w <= 0.0f) {
            out[i] = a[i];
        } else if (w >= 1.0f) {
            out[i] = b[i];
        } else {
            out[i] = lerp(a[i], b[i], w);
        }
    }
}

// ---- additive -----------------------------------------------------------------------------------
Transform make_additive(const Transform& pose, const Transform& reference) noexcept {
    return make_transform(pose.position - reference.position,
                          normalize(pose.rotation * conjugate(reference.rotation)),
                          pose.scale * safe_reciprocal(reference.scale));
}

Transform apply_additive(const Transform& base, const Transform& delta, f32 weight) noexcept {
    if (weight <= 0.0f) {
        return base;
    }
    const Quat r = weight >= 1.0f ? delta.rotation : nlerp(quat_identity(), delta.rotation, weight);
    const Vec3 s = Vec3(1.0f) + (delta.scale - Vec3(1.0f)) * weight;
    return make_transform(base.position + delta.position * weight, normalize(r * base.rotation),
                          base.scale * s);
}

void make_additive(std::span<const Transform> pose, std::span<const Transform> reference,
                   std::span<Transform> out_delta) {
    const usize n = out_delta.size();
    AE_ASSERT(pose.size() >= n && reference.size() >= n);
    for (usize i = 0; i < n; ++i) {
        out_delta[i] = make_additive(pose[i], reference[i]);
    }
}

void apply_additive(std::span<const Transform> base, std::span<const Transform> delta, f32 weight,
                    std::span<Transform> out) {
    const usize n = out.size();
    AE_ASSERT(base.size() >= n && delta.size() >= n);
    for (usize i = 0; i < n; ++i) {
        out[i] = apply_additive(base[i], delta[i], weight);
    }
}

void apply_additive_masked(std::span<const Transform> base, std::span<const Transform> delta,
                           f32 weight, const BoneMask& mask, std::span<Transform> out) {
    const usize n = out.size();
    AE_ASSERT(base.size() >= n && delta.size() >= n);
    const auto weights = mask.weights();
    for (usize i = 0; i < n; ++i) {
        const f32 w = i < weights.size() ? weight * weights[i] : 0.0f;
        out[i] = apply_additive(base[i], delta[i], w);
    }
}

} // namespace aether::animation
