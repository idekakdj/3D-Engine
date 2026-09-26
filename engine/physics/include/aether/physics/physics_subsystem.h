// aether/physics/physics_subsystem.h — engine subsystem that owns the PhysicsWorld.
//
// Frame integration (blueprint §5.1):
//   on_startup       acquire the Jolt runtime; bind EngineContext::world if already set
//   on_begin_frame   accumulate frame delta; clear last frame's contact events
//   on_fixed_update  (0..N per frame) rebind if the active World changed, then step(fixed_delta)
//   on_update        compute the render interpolation alpha; rebuild debug lines if enabled
//   on_shutdown      destroy the PhysicsWorld and release the Jolt runtime
//
// Contact events therefore accumulate over all fixed steps of a frame and stay readable until
// the next on_begin_frame. Debug lines are NOT pushed into RenderScene here (physics must not
// depend on the renderer); gameplay copies debug_lines() into RenderScene::debug_lines.
//
// Thread affinity: main thread only.
#pragma once

#include "aether/core/subsystem.h"
#include "aether/core/types.h"
#include "aether/physics/physics_types.h"
#include "aether/physics/physics_world.h"

#include <memory>
#include <vector>

namespace aether::physics {

class PhysicsSubsystem final : public ISubsystem {
public:
    explicit PhysicsSubsystem(const PhysicsWorldSettings& settings = {});
    ~PhysicsSubsystem() override;

    PhysicsSubsystem(const PhysicsSubsystem&) = delete;
    PhysicsSubsystem& operator=(const PhysicsSubsystem&) = delete;

    [[nodiscard]] const char* name() const override { return "Physics"; }

    void on_startup(EngineContext& ctx) override;
    void on_shutdown() override;
    void on_begin_frame(FrameContext& ctx) override;
    void on_fixed_update(FixedContext& ctx) override;
    void on_update(FrameContext& ctx) override;

    // (Re)binds physics to `world` (nullptr unbinds). Recreates the PhysicsWorld, which
    // re-creates bodies from the ECS components on its first step.
    void bind_world(World* world);

    [[nodiscard]] PhysicsWorld*       physics_world() { return physics_world_.get(); }
    [[nodiscard]] const PhysicsWorld* physics_world() const { return physics_world_.get(); }
    [[nodiscard]] bool                is_started() const { return started_; }

    // Render interpolation factor in [0,1] (accumulator / fixed_dt) for the current frame.
    [[nodiscard]] f32 interpolation_alpha() const { return alpha_; }
    // Lets the Application override alpha with its own accumulator (authoritative if it clamps
    // or drops steps). Cleared again by the next on_update.
    void set_interpolation_alpha(f32 alpha);

    // Standalone driver for hosts without the Application loop (tools, tests): accumulates
    // `frame_dt`, runs up to `max_steps` fixed steps of `fixed_dt` (excess time is dropped to
    // avoid a spiral of death) and updates the interpolation alpha. Returns the steps taken.
    u32 advance(f32 frame_dt, f32 fixed_dt = 1.0f / 60.0f, u32 max_steps = 8);

    // Debug lines rebuilt every on_update()/advance() while flags != None.
    void set_debug_draw(DebugDrawFlags flags) { debug_flags_ = flags; }
    [[nodiscard]] DebugDrawFlags                debug_draw() const { return debug_flags_; }
    [[nodiscard]] const std::vector<DebugLine>& debug_lines() const { return debug_lines_; }

private:
    void refresh_debug_lines();

    PhysicsWorldSettings          settings_;
    std::unique_ptr<PhysicsWorld> physics_world_;
    World*                        bound_world_ = nullptr;
    EngineContext*                engine_ = nullptr;
    bool                          started_ = false;

    f64  accumulator_ = 0.0;
    f32  fixed_dt_ = 1.0f / 60.0f;
    f32  alpha_ = 0.0f;
    bool alpha_overridden_ = false;

    DebugDrawFlags         debug_flags_ = DebugDrawFlags::None;
    std::vector<DebugLine> debug_lines_;
};

} // namespace aether::physics
