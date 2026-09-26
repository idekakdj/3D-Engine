// aether/gameplay/asset_hot_reload.h — per-frame asset and shader hot reload.
//
// AssetHotReloadSubsystem (SubsystemKind::Engine) calls AssetManager::poll_file_changes() every
// frame (the manager rate-limits its file sweeps). Reloaded assets bump their data version,
// which RenderResourceCache and AnimationAssetBinder pick up on their own. Optionally, a key
// (F5 by default) recompiles the renderer's shaders; request_shader_reload() does the same
// from code (editor menu).
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/input.h"
#include "aether/core/subsystem.h"
#include "aether/core/types.h"

namespace aether::assets {
class AssetManager;
}

namespace aether::gameplay {

class AssetHotReloadSubsystem final : public ISubsystem {
public:
    explicit AssetHotReloadSubsystem(assets::AssetManager* assets, Key shader_reload_key = Key::F5);

    [[nodiscard]] const char* name() const override { return "AssetHotReload"; }
    void on_startup(EngineContext& ctx) override;
    void on_shutdown() override; // removes the reload listener
    void on_update(FrameContext& ctx) override;

    void request_shader_reload() { shader_reload_requested_ = true; }
    // Key::Unknown disables the shortcut.
    void set_shader_reload_key(Key key) { shader_key_ = key; }

    [[nodiscard]] u64 reloaded_assets() const noexcept { return reloaded_; }
    [[nodiscard]] u32 shader_reloads() const noexcept { return shader_reloads_; }

private:
    assets::AssetManager* assets_ = nullptr;
    EngineContext*        engine_ = nullptr;
    Key                   shader_key_;
    bool                  shader_reload_requested_ = false;
    u64                   listener_ = 0;
    u64                   reloaded_ = 0;
    u32                   shader_reloads_ = 0;
};

} // namespace aether::gameplay
