// skeleton.cpp — Skeleton construction/validation from assets::SkeletonData.
#include "aether/animation/skeleton.h"

#include "aether/animation/anim_math.h"
#include "aether/assets/asset_types.h"
#include "aether/core/log.h"

#include <format>

namespace aether::animation {

Result<Skeleton> Skeleton::create(const assets::SkeletonData& data) {
    const usize count = data.parents.size();
    if (count == 0) {
        return make_error<Skeleton>(ErrorCode::InvalidArgument, "Skeleton: no joints");
    }
    if (count > kMaxJoints) {
        return make_error<Skeleton>(
            ErrorCode::InvalidArgument,
            std::format("Skeleton: {} joints exceeds the limit of {}", count, kMaxJoints));
    }
    if (data.bind_local.size() != count) {
        return make_error<Skeleton>(ErrorCode::InvalidArgument,
                                    std::format("Skeleton: {} parents but {} bind transforms", count,
                                                data.bind_local.size()));
    }
    if (!data.joint_names.empty() && data.joint_names.size() != count) {
        return make_error<Skeleton>(ErrorCode::InvalidArgument,
                                    std::format("Skeleton: {} parents but {} joint names", count,
                                                data.joint_names.size()));
    }
    if (!data.inverse_bind.empty() && data.inverse_bind.size() != count) {
        return make_error<Skeleton>(ErrorCode::InvalidArgument,
                                    std::format("Skeleton: {} parents but {} inverse bind matrices",
                                                count, data.inverse_bind.size()));
    }
    for (usize i = 0; i < count; ++i) {
        const i32 p = data.parents[i];
        if (p < -1 || p >= static_cast<i32>(i)) {
            return make_error<Skeleton>(
                ErrorCode::InvalidArgument,
                std::format("Skeleton: joint {} has parent {} (joints must be topologically "
                            "sorted: parents[i] < i)",
                            i, p));
        }
    }

    const u32 n = static_cast<u32>(count);
    Skeleton  s;
    s.parents_ = data.parents;
    s.names_.resize(n);
    s.bind_local_.resize(n);
    s.bind_model_.resize(n);
    s.bind_model_tr_.resize(n);
    for (u32 i = 0; i < n; ++i) {
        const bool unnamed = data.joint_names.empty() || data.joint_names[i].empty();
        s.names_[i] = unnamed ? std::format("joint_{}", i) : data.joint_names[i];

        Transform local = data.bind_local[i];
        local.rotation = normalize(local.rotation);
        s.bind_local_[i] = local;

        const i32  p = s.parents_[i];
        const Mat4 m = to_mat4(local);
        s.bind_model_[i] = p < 0 ? m : s.bind_model_[static_cast<u32>(p)] * m;
        s.bind_model_tr_[i] = p < 0 ? local : compose(s.bind_model_tr_[static_cast<u32>(p)], local);
    }

    if (data.inverse_bind.empty()) {
        s.inverse_bind_.resize(n);
        for (u32 i = 0; i < n; ++i) {
            s.inverse_bind_[i] = inverse(s.bind_model_[i]);
        }
    } else {
        s.inverse_bind_ = data.inverse_bind;
    }

    u32 duplicates = 0;
    s.name_to_index_.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        if (!s.name_to_index_.try_emplace(s.names_[i], i).second) {
            ++duplicates;
        }
    }
    if (duplicates != 0) {
        AE_LOG_WARN("Animation", "Skeleton: {} duplicate joint name(s); find_joint returns the first",
                    duplicates);
    }
    return s;
}

u32 Skeleton::find_joint(std::string_view name) const noexcept {
    const auto it = name_to_index_.find(name);
    return it == name_to_index_.end() ? kInvalidJoint : it->second;
}

bool Skeleton::is_ancestor(u32 ancestor, u32 joint) const noexcept {
    if (joint >= joint_count()) {
        return false;
    }
    for (i32 p = parents_[joint]; p >= 0; p = parents_[static_cast<u32>(p)]) {
        if (static_cast<u32>(p) == ancestor) {
            return true;
        }
    }
    return false;
}

} // namespace aether::animation
