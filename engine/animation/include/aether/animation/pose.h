// aether/animation/pose.h — local-space poses and the pose math on them.
//
// A local-space pose is one Transform per joint (relative to its parent), in skeleton order.
// The free functions operate on caller-owned spans and never allocate; `Pose` is a small owning
// wrapper for convenience. Unless stated otherwise `out` may alias any input (all operations are
// element-wise), and every span must hold at least the relevant joint count.
//
// Blending: translation/scale lerp, rotation nlerp with hemisphere correction (shortest arc).
// Additive (Unreal "local space" convention):
//     delta.position = pose.position - ref.position
//     delta.rotation = pose.rotation * inverse(ref.rotation)     (pre-multiplied, parent space)
//     delta.scale    = pose.scale / ref.scale
//   apply(base, delta, w): position += w * delta.position,
//                          rotation  = nlerp(identity, delta.rotation, w) * rotation,
//                          scale    *= lerp(1, delta.scale, w).
//   The identity Transform is the additive identity, and apply(ref, delta(pose, ref), 1) == pose.
//
// Thread-safety: free functions are thread-safe on disjoint data. BoneMask / Pose are plain
// values (const methods safe concurrently).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <span>
#include <string_view>
#include <vector>

namespace aether::animation {

class Skeleton;

// Per-joint blend weights in [0, 1] (e.g. "upper body" = 1 on spine and below, 0 elsewhere).
class BoneMask {
public:
    BoneMask() = default;
    explicit BoneMask(u32 joint_count, f32 fill = 0.0f) : weights_(joint_count, fill) {}

    // Mask with `weight` on `joint` and all of its descendants, 0 elsewhere.
    [[nodiscard]] static BoneMask from_branch(const Skeleton& skeleton, u32 joint,
                                              f32 weight = 1.0f);
    // By joint name; returns an all-zero mask (and logs) if the name is unknown.
    [[nodiscard]] static BoneMask from_branch(const Skeleton& skeleton, std::string_view joint,
                                              f32 weight = 1.0f);

    // Sets `weight` on `joint` and every descendant (other joints unchanged).
    void set_branch(const Skeleton& skeleton, u32 joint, f32 weight);
    void set(u32 joint, f32 weight) { weights_[joint] = weight; }

    [[nodiscard]] f32 weight(u32 joint) const noexcept {
        return joint < weights_.size() ? weights_[joint] : 0.0f;
    }
    [[nodiscard]] u32 joint_count() const noexcept { return static_cast<u32>(weights_.size()); }
    [[nodiscard]] bool empty() const noexcept { return weights_.empty(); }
    [[nodiscard]] std::span<const f32> weights() const noexcept { return weights_; }

private:
    std::vector<f32> weights_;
};

// Owning local-space pose.
class Pose {
public:
    Pose() = default;
    explicit Pose(u32 joint_count) : local_(joint_count) {} // identity transforms
    explicit Pose(const Skeleton& skeleton);                // bind pose

    void set_bind(const Skeleton& skeleton);
    void set_identity();
    void resize(u32 joint_count) { local_.resize(joint_count); }

    [[nodiscard]] u32 joint_count() const noexcept { return static_cast<u32>(local_.size()); }
    [[nodiscard]] std::span<Transform> local() noexcept { return local_; }
    [[nodiscard]] std::span<const Transform> local() const noexcept { return local_; }
    Transform&       operator[](u32 joint) noexcept { return local_[joint]; }
    const Transform& operator[](u32 joint) const noexcept { return local_[joint]; }

private:
    std::vector<Transform> local_;
};

// ---- hierarchy ----------------------------------------------------------------------------------
// model[j] = model[parent(j)] * matrix(local[j]) (roots: matrix(local[j])). `out_model` must not
// alias anything else.
void local_to_model(const Skeleton& skeleton, std::span<const Transform> local,
                    std::span<Mat4> out_model);
// Model-space TRS of one joint, walking its parent chain (O(depth)).
[[nodiscard]] Transform model_transform(const Skeleton& skeleton, std::span<const Transform> local,
                                        u32 joint);
// palette[j] = model[j] * inverse_bind[j] — the skinning matrices the renderer consumes.
void compute_skinning_palette(std::span<const Mat4> model, std::span<const Mat4> inverse_bind,
                              std::span<Mat4> out_palette);

// ---- blending -----------------------------------------------------------------------------------
// out = lerp(a, b, weight); weight <= 0 copies a, weight >= 1 copies b exactly.
void blend_poses(std::span<const Transform> a, std::span<const Transform> b, f32 weight,
                 std::span<Transform> out);
// Per-joint weight = weight * mask[j].
void blend_poses_masked(std::span<const Transform> a, std::span<const Transform> b, f32 weight,
                        const BoneMask& mask, std::span<Transform> out);

// ---- additive -----------------------------------------------------------------------------------
void make_additive(std::span<const Transform> pose, std::span<const Transform> reference,
                   std::span<Transform> out_delta);
void apply_additive(std::span<const Transform> base, std::span<const Transform> delta, f32 weight,
                    std::span<Transform> out);
void apply_additive_masked(std::span<const Transform> base, std::span<const Transform> delta,
                           f32 weight, const BoneMask& mask, std::span<Transform> out);

// Single-transform versions of the above.
[[nodiscard]] Transform make_additive(const Transform& pose, const Transform& reference) noexcept;
[[nodiscard]] Transform apply_additive(const Transform& base, const Transform& delta,
                                       f32 weight) noexcept;

} // namespace aether::animation
