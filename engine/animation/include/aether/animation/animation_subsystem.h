// aether/animation/animation_subsystem.h — ticks every AnimatorComponent each frame.
//
// on_update(frame): gathers the world's animators (main thread), then runs each animator's full
// pipeline (graph update -> evaluate -> root motion -> IK -> model pose -> palette) as one job via
// JobSystem::parallel_for, and finally (for RootMotionMode::ApplyToTransform) moves the entity's
// TransformComponent by the extracted delta and marks it dirty. Small batches, or an
// uninitialized JobSystem, run inline on the calling thread.
//
// The gameplay layer copies palette(entity) into RenderScene::joint_matrices (animation does not
// depend on the renderer). World-space IK targets use TransformComponent::world from the last
// World::update_transforms().
//
// Thread-safety: main thread only (internally parallel).
#pragma once

#include "aether/core/math.h"
#include "aether/core/subsystem.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <span>
#include <vector>

namespace aether {
class World;
struct TransformComponent;
} // namespace aether

namespace aether::animation {

class AnimatorComponent;

struct AnimationSubsystemSettings {
    u32 parallel_threshold = 4; // fewer animators than this are ticked inline
    u32 group_size = 4;         // animators per job
};

class AnimationSubsystem final : public ISubsystem {
public:
    AnimationSubsystem() = default;
    explicit AnimationSubsystem(const AnimationSubsystemSettings& settings) : settings_(settings) {}

    [[nodiscard]] const char* name() const override { return "Animation"; }
    void on_startup(EngineContext& context) override;
    void on_shutdown() override;
    void on_update(FrameContext& frame) override; // ticks context.engine->world (if any)

    // Explicit entry point (tools, tests, custom loops). Main thread only.
    void update(World& world, f32 dt);

    // Skinning palette of `entity` from the last update (empty if it has no animator).
    [[nodiscard]] std::span<const Mat4> palette(Entity entity) const;
    [[nodiscard]] u32 animated_count() const noexcept { return last_count_; }

private:
    struct WorkItem {
        AnimatorComponent*  animator = nullptr;
        TransformComponent* transform = nullptr;
    };
    void process(const WorkItem& item, f32 dt) const;

    AnimationSubsystemSettings settings_{};
    EngineContext*             engine_ = nullptr;
    World*                     world_ = nullptr;
    std::vector<WorkItem>      work_;
    u32                        last_count_ = 0;
};

} // namespace aether::animation
