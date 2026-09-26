// scripting_subsystem.cpp — drives the bound World's ScriptVM from the engine frame loop.
#include "aether/scripting/scripting_subsystem.h"

#include "aether/core/input.h"
#include "aether/core/log.h"

#include <utility>

namespace aether::scripting {

ScriptingSubsystem::ScriptingSubsystem(ScriptVMConfig config) : config_(std::move(config)) {}

ScriptingSubsystem::~ScriptingSubsystem() { vm_.reset(); }

void ScriptingSubsystem::on_startup(EngineContext& ctx) { sync_context(&ctx); }

void ScriptingSubsystem::on_shutdown() {
    vm_.reset(); // on_destroy runs for every started instance while the world is still alive
    world_  = nullptr;
    window_ = nullptr;
}

void ScriptingSubsystem::on_fixed_update(FixedContext& ctx) {
    sync_context(ctx.engine);
    if (vm_) {
        vm_->fixed_update(ctx.fixed_delta);
    }
}

void ScriptingSubsystem::on_update(FrameContext& ctx) {
    sync_context(ctx.engine);
    if (vm_) {
        vm_->set_input(window_ != nullptr ? &Input::state() : nullptr);
        vm_->update(ctx.time.delta);
    }
}

void ScriptingSubsystem::add_binding_registrar(BindingRegistrar registrar) {
    if (!registrar) {
        return;
    }
    registrars_.push_back(std::move(registrar));
    if (vm_) {
        registrars_.back()(*vm_);
    }
}

void ScriptingSubsystem::bind_world(World* world) {
    if (world == world_ && (vm_ != nullptr || world == nullptr)) {
        return;
    }
    vm_.reset();
    world_ = world;
    if (world_ == nullptr) {
        return;
    }
    vm_ = std::make_unique<ScriptVM>(*world_, config_);
    for (const BindingRegistrar& r : registrars_) {
        r(*vm_);
    }
    AE_LOG_DEBUG("Script", "ScriptVM created (script root: {})", vm_->script_root().generic_string());
}

void ScriptingSubsystem::sync_context(EngineContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    window_ = ctx->window;
    if (ctx->world != world_ || (world_ != nullptr && vm_ == nullptr)) {
        bind_world(ctx->world);
    }
}

} // namespace aether::scripting
