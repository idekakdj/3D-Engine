// components.cpp — AnimatorComponent per-frame pipeline.
#include "aether/animation/components.h"

#include "aether/animation/anim_math.h"
#include "aether/animation/pose.h"

#include <algorithm>

namespace aether::animation {

AnimatorComponent::AnimatorComponent(std::shared_ptr<const Skeleton> skeleton,
                                     std::shared_ptr<const AnimGraph> graph) {
    set_skeleton(std::move(skeleton));
    if (graph) {
        set_graph(std::move(graph));
    }
}

void AnimatorComponent::set_skeleton(std::shared_ptr<const Skeleton> skeleton) {
    skeleton_ = std::move(skeleton);
    if (instance_.valid() && (!skeleton_ || &instance_.graph()->skeleton() != skeleton_.get())) {
        instance_ = AnimGraphInstance{};
    }
    reset_outputs();
}

bool AnimatorComponent::set_graph(std::shared_ptr<const AnimGraph> graph) {
    if (!graph) {
        instance_ = AnimGraphInstance{};
        reset_outputs();
        return true;
    }
    if (skeleton_ && graph->skeleton_ptr() != skeleton_) {
        return false;
    }
    if (!skeleton_) {
        skeleton_ = graph->skeleton_ptr();
    }
    instance_ = AnimGraphInstance(std::move(graph));
    reset_outputs();
    return true;
}

void AnimatorComponent::reset_outputs() {
    root_motion_delta_ = {};
    if (!skeleton_) {
        local_.clear();
        model_.clear();
        palette_.clear();
        return;
    }
    const auto bind = skeleton_->bind_pose();
    local_.assign(bind.begin(), bind.end());
    model_.resize(bind.size());
    palette_.resize(bind.size());
    local_to_model(*skeleton_, local_, model_);
    compute_skinning_palette(model_, skeleton_->inverse_bind(), palette_);
}

u32 AnimatorComponent::add_two_bone_ik(const TwoBoneIKChain& chain) {
    two_bone_ik_.push_back(chain);
    return static_cast<u32>(two_bone_ik_.size() - 1);
}

u32 AnimatorComponent::add_look_at(const LookAtChain& chain) {
    look_at_.push_back(chain);
    return static_cast<u32>(look_at_.size() - 1);
}

void AnimatorComponent::clear_ik() noexcept {
    two_bone_ik_.clear();
    look_at_.clear();
}

void AnimatorComponent::tick(f32 dt, const Mat4* entity_world) {
    if (!skeleton_) {
        return;
    }
    const Skeleton&           sk = *skeleton_;
    const f32                 step = paused_ ? 0.0f : dt * playback_rate_;
    const RootMotionSettings* rm = root_motion_mode_ != RootMotionMode::Ignore &&
                                           root_motion_settings_.joint < sk.joint_count()
                                       ? &root_motion_settings_
                                       : nullptr;
    if (instance_.valid()) {
        instance_.update(step, rm);
        instance_.evaluate(local_);
        root_motion_delta_ = rm != nullptr ? instance_.root_motion() : RootMotionDelta{};
    } else {
        std::copy(sk.bind_pose().begin(), sk.bind_pose().end(), local_.begin());
        root_motion_delta_ = {};
    }
    if (rm != nullptr) {
        remove_root_motion(sk, *rm, local_);
    }

    // World-space IK targets -> model space (inverse computed once, only when needed).
    Mat4 world_to_model(1.0f);
    Quat world_to_model_rot = quat_identity();
    bool have_inverse = false;
    auto to_model = [&](IKTargetSpace space) -> bool {
        if (space == IKTargetSpace::Model) {
            return false;
        }
        if (!have_inverse && entity_world != nullptr) {
            world_to_model = inverse(*entity_world);
            world_to_model_rot = conjugate(rotation_from_matrix(*entity_world));
            have_inverse = true;
        }
        return true;
    };
    for (const TwoBoneIKChain& chain : two_bone_ik_) {
        if (!chain.enabled || chain.settings.weight <= 0.0f) {
            continue;
        }
        TwoBoneIKSettings s = chain.settings;
        if (to_model(chain.space)) {
            s.target = transform_point(world_to_model, s.target);
            s.pole = transform_point(world_to_model, s.pole);
            s.target_rotation = normalize(world_to_model_rot * s.target_rotation);
        }
        solve_two_bone_ik(sk, local_, s);
    }
    for (const LookAtChain& chain : look_at_) {
        if (!chain.enabled || chain.settings.weight <= 0.0f) {
            continue;
        }
        LookAtSettings s = chain.settings;
        if (to_model(chain.space)) {
            s.target = transform_point(world_to_model, s.target);
        }
        solve_look_at(sk, local_, s);
    }

    local_to_model(sk, local_, model_);
    compute_skinning_palette(model_, sk.inverse_bind(), palette_);
}

} // namespace aether::animation
