// aether/animation/skeleton.h — runtime skeleton: joint hierarchy, bind pose, inverse binds.
//
// Built once from assets::SkeletonData (importer output) and then shared immutably
// (std::shared_ptr<const Skeleton>) by every clip, graph and animator targeting it. Joints are
// kept in topological order (parents[i] < i), so every hierarchy pass (local->model, masks, IK)
// is a single forward loop.
//
// "Model space" throughout the animation module means the skeleton's root space (the space of
// the entity that owns the animator). Skinning palettes are model[j] * inverse_bind[j].
//
// Thread-safety: immutable after create(); all const members may be called concurrently.
#pragma once

#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aether::assets {
struct SkeletonData;
}

namespace aether::animation {

inline constexpr u32 kInvalidJoint = kInvalidU32;
// core/geometry.h SkinVertex stores 16-bit joint indices, which bounds the skeleton size.
inline constexpr u32 kMaxJoints = 65535;

namespace detail {
// Transparent hash so string-keyed maps can be queried with std::string_view (no allocation).
struct StringHash {
    using is_transparent = void;
    usize operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
template <typename V>
using StringMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;
} // namespace detail

class Skeleton {
public:
    // Validates and converts importer data:
    //  * parents / bind_local sizes match; joint_names is empty or one per joint (empty names
    //    become "joint_<i>");
    //  * topological order: parents[i] == -1 or 0 <= parents[i] < i;
    //  * inverse_bind is empty (computed from the bind pose) or one per joint.
    // Bind rotations are renormalized. Duplicate names are allowed (find_joint returns the first).
    [[nodiscard]] static Result<Skeleton> create(const assets::SkeletonData& data);

    [[nodiscard]] u32 joint_count() const noexcept { return static_cast<u32>(parents_.size()); }
    [[nodiscard]] i32 parent(u32 joint) const noexcept { return parents_[joint]; }
    [[nodiscard]] std::span<const i32> parents() const noexcept { return parents_; }
    [[nodiscard]] const std::string& joint_name(u32 joint) const noexcept { return names_[joint]; }
    [[nodiscard]] std::span<const std::string> joint_names() const noexcept { return names_; }
    // Index of the first joint named `name`, or kInvalidJoint. No allocation.
    [[nodiscard]] u32 find_joint(std::string_view name) const noexcept;

    // Local-space bind (rest) pose, one Transform per joint.
    [[nodiscard]] std::span<const Transform> bind_pose() const noexcept { return bind_local_; }
    // Model-space bind matrices / transforms (computed from the bind pose).
    [[nodiscard]] std::span<const Mat4> bind_model() const noexcept { return bind_model_; }
    [[nodiscard]] const Transform& bind_model_transform(u32 joint) const noexcept {
        return bind_model_tr_[joint];
    }
    // Model-space inverse bind matrices (from the asset, or inverse(bind_model) if absent).
    [[nodiscard]] std::span<const Mat4> inverse_bind() const noexcept { return inverse_bind_; }

    // True if `ancestor` is a strict ancestor of `joint`.
    [[nodiscard]] bool is_ancestor(u32 ancestor, u32 joint) const noexcept;

private:
    Skeleton() = default;

    std::vector<std::string>        names_;
    std::vector<i32>                parents_;
    std::vector<Transform>          bind_local_;
    std::vector<Transform>          bind_model_tr_;
    std::vector<Mat4>               bind_model_;
    std::vector<Mat4>               inverse_bind_;
    detail::StringMap<u32>          name_to_index_;
};

} // namespace aether::animation
