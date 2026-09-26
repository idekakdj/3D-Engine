// animation_subsystem.cpp — parallel per-frame animator ticking.
#include "aether/animation/animation_subsystem.h"

#include "aether/animation/components.h"
#include "aether/core/job_system.h"
#include "aether/scene/components.h"
#include "aether/scene/world.h"

#include <algorithm>

namespace aether::animation {

void AnimationSubsystem::on_startup(EngineContext& context) { engine_ = &context; }

void AnimationSubsystem::on_shutdown() {
    engine_ = nullptr;
    world_ = nullptr;
    work_.clear();
    work_.shrink_to_fit();
}

void AnimationSubsystem::on_update(FrameContext& frame) {
    EngineContext* engine = frame.engine != nullptr ? frame.engine : engine_;
    if (engine != nullptr && engine->world != nullptr) {
        update(*engine->world, frame.time.delta);
    }
}

void AnimationSubsystem::process(const WorkItem& item, f32 dt) const {
    const Mat4* world = item.transform != nullptr ? &item.transform->world : nullptr;
    item.animator->tick(dt, world);
    if (item.transform != nullptr &&
        item.animator->root_motion_mode() == RootMotionMode::ApplyToTransform) {
        apply_root_motion(item.transform->local, item.animator->root_motion_delta());
        item.transform->dirty = true;
    }
}

void AnimationSubsystem::update(World& world, f32 dt) {
    world_ = &world;
    work_.clear(); // keeps capacity: steady state does not allocate
    world.registry().view<AnimatorComponent>().each([&](Entity e, AnimatorComponent& animator) {
        if (animator.enabled() && animator.skeleton()) {
            work_.push_back({&animator, world.try_get<TransformComponent>(e)});
        }
    });
    const u32 count = static_cast<u32>(work_.size());
    last_count_ = count;
    if (count == 0) {
        return;
    }
    if (count >= settings_.parallel_threshold && JobSystem::worker_count() > 0) {
        JobCounter counter;
        JobSystem::parallel_for(count, std::max(settings_.group_size, 1u),
                                [this, dt](u32 i) { process(work_[i], dt); }, &counter);
        JobSystem::wait(counter);
    } else {
        for (const WorkItem& item : work_) {
            process(item, dt);
        }
    }
}

std::span<const Mat4> AnimationSubsystem::palette(Entity entity) const {
    if (world_ == nullptr || !world_->valid(entity)) {
        return {};
    }
    const auto* animator = world_->try_get<AnimatorComponent>(entity);
    return animator != nullptr ? animator->palette() : std::span<const Mat4>{};
}

} // namespace aether::animation
