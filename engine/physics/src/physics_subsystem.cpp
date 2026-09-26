// physics_subsystem.cpp — see aether/physics/physics_subsystem.h.
#include "aether/physics/physics_subsystem.h"

#include "aether/core/log.h"
#include "aether/physics/runtime.h"

#include <algorithm>

namespace aether::physics {

PhysicsSubsystem::PhysicsSubsystem(const PhysicsWorldSettings& settings) : settings_(settings) {}

PhysicsSubsystem::~PhysicsSubsystem() {
    if (started_)
        on_shutdown();
}

void PhysicsSubsystem::on_startup(EngineContext& ctx) {
    if (started_)
        return;
    runtime_acquire();
    started_ = true;
    engine_ = &ctx;
    bind_world(ctx.world);
}

void PhysicsSubsystem::on_shutdown() {
    if (!started_)
        return;
    physics_world_.reset(); // before releasing the runtime: it owns Jolt objects
    bound_world_ = nullptr;
    engine_ = nullptr;
    debug_lines_.clear();
    started_ = false;
    runtime_release();
}

void PhysicsSubsystem::bind_world(World* world) {
    if (world == bound_world_ && (world == nullptr || physics_world_ != nullptr))
        return;
    physics_world_.reset();
    bound_world_ = world;
    accumulator_ = 0.0;
    alpha_ = 0.0f;
    if (world != nullptr) {
        physics_world_ = std::make_unique<PhysicsWorld>(*world, settings_);
        AE_LOG_INFO("Physics", "physics bound to world");
    }
}

void PhysicsSubsystem::on_begin_frame(FrameContext& ctx) {
    accumulator_ += static_cast<f64>(std::max(ctx.time.delta, 0.0f));
    if (physics_world_ != nullptr)
        physics_world_->clear_contact_events(); // previous frame's events have been consumed
}

void PhysicsSubsystem::on_fixed_update(FixedContext& ctx) {
    if (!started_)
        return;
    World* active = ctx.engine != nullptr ? ctx.engine->world : bound_world_;
    if (active != bound_world_)
        bind_world(active);
    if (physics_world_ == nullptr)
        return;
    fixed_dt_ = ctx.fixed_delta > 0.0f ? ctx.fixed_delta : fixed_dt_;
    physics_world_->step(fixed_dt_);
    accumulator_ -= static_cast<f64>(fixed_dt_);
}

void PhysicsSubsystem::on_update(FrameContext& ctx) {
    if (ctx.time.fixed_delta > 0.0f)
        fixed_dt_ = ctx.time.fixed_delta;
    // The Application may have dropped steps (spiral-of-death clamp): keep our mirror of its
    // accumulator inside [0, fixed_dt).
    accumulator_ = std::clamp(accumulator_, 0.0, static_cast<f64>(fixed_dt_));
    if (!alpha_overridden_)
        alpha_ = static_cast<f32>(std::clamp(accumulator_ / static_cast<f64>(fixed_dt_), 0.0, 1.0));
    alpha_overridden_ = false;
    refresh_debug_lines();
}

void PhysicsSubsystem::set_interpolation_alpha(f32 alpha) {
    alpha_ = std::clamp(alpha, 0.0f, 1.0f);
    alpha_overridden_ = true;
}

u32 PhysicsSubsystem::advance(f32 frame_dt, f32 fixed_dt, u32 max_steps) {
    if (physics_world_ == nullptr || !(fixed_dt > 0.0f))
        return 0;
    fixed_dt_ = fixed_dt;
    physics_world_->clear_contact_events();
    accumulator_ += static_cast<f64>(std::max(frame_dt, 0.0f));
    u32 steps = 0;
    while (accumulator_ >= static_cast<f64>(fixed_dt) && steps < max_steps) {
        physics_world_->step(fixed_dt);
        accumulator_ -= static_cast<f64>(fixed_dt);
        ++steps;
    }
    accumulator_ = std::clamp(accumulator_, 0.0, static_cast<f64>(fixed_dt)); // drop excess
    alpha_ = static_cast<f32>(accumulator_ / static_cast<f64>(fixed_dt));
    refresh_debug_lines();
    return steps;
}

void PhysicsSubsystem::refresh_debug_lines() {
    debug_lines_.clear();
    if (physics_world_ != nullptr && debug_flags_ != DebugDrawFlags::None)
        physics_world_->build_debug_lines(debug_lines_, debug_flags_);
}

} // namespace aether::physics
