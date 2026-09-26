// aether/scripting/scripting_subsystem.h — ISubsystem driving the World's ScriptVM.
//
// Creates a ScriptVM for EngineContext::world at startup (and re-creates it whenever the
// context's world pointer changes), forwards on_fixed_update/on_update to it, and feeds it the
// core Input snapshot when a window exists. ScriptComponent add/remove/replace is followed
// through registry signals (ScriptVMConfig::auto_attach), so instances appear and disappear
// automatically. on_shutdown destroys the VM (on_destroy runs for every started instance), which
// makes Application::reload_world() / play-in-editor restore safe.
//
// Layer-5 glue that registers extra bindings (e.g. physics raycasts through lua_integration.h)
// adds a registrar: it runs for every VM this subsystem creates, before any script loads.
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/subsystem.h"
#include "aether/scripting/script_vm.h"

#include <functional>
#include <memory>
#include <vector>

namespace aether::scripting {

class ScriptingSubsystem final : public ISubsystem {
public:
    using BindingRegistrar = std::function<void(ScriptVM&)>;

    explicit ScriptingSubsystem(ScriptVMConfig config = {});
    ~ScriptingSubsystem() override;

    ScriptingSubsystem(const ScriptingSubsystem&)            = delete;
    ScriptingSubsystem& operator=(const ScriptingSubsystem&) = delete;

    [[nodiscard]] const char* name() const override { return "Scripting"; }

    void on_startup(EngineContext& ctx) override;
    void on_shutdown() override;
    void on_fixed_update(FixedContext& ctx) override;
    void on_update(FrameContext& ctx) override;

    // The VM of the bound world (null before startup / without a world).
    [[nodiscard]] ScriptVM* vm() noexcept { return vm_.get(); }

    // Runs `registrar` for the current VM (if any) and every VM created later.
    void add_binding_registrar(BindingRegistrar registrar);

    // Binds a different world (null unbinds). Destroys the current VM first.
    void bind_world(World* world);

private:
    void sync_context(EngineContext* ctx);

    ScriptVMConfig                config_;
    std::vector<BindingRegistrar> registrars_;
    World*                        world_  = nullptr;
    Window*                       window_ = nullptr;
    std::unique_ptr<ScriptVM>     vm_;
};

} // namespace aether::scripting
