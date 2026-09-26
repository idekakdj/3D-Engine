// asset_hot_reload.cpp — AssetHotReloadSubsystem (see asset_hot_reload.h).
#include "aether/gameplay/asset_hot_reload.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/renderer/renderer.h"

namespace aether::gameplay {

AssetHotReloadSubsystem::AssetHotReloadSubsystem(assets::AssetManager* assets, Key shader_reload_key)
    : assets_(assets), shader_key_(shader_reload_key) {}

void AssetHotReloadSubsystem::on_startup(EngineContext& ctx) {
    engine_ = &ctx;
    if (assets_ != nullptr && listener_ == 0) {
        listener_ = assets_->on_reloaded([this](AssetId id) {
            ++reloaded_;
            AE_LOG_INFO("Assets", "hot reloaded {}", id.to_string());
        });
    }
}

void AssetHotReloadSubsystem::on_shutdown() {
    if (assets_ != nullptr && listener_ != 0) {
        assets_->remove_reload_listener(listener_);
    }
    listener_ = 0;
    engine_   = nullptr;
}

void AssetHotReloadSubsystem::on_update(FrameContext& ctx) {
    EngineContext* engine = ctx.engine != nullptr ? ctx.engine : engine_;
    if (assets_ != nullptr && assets_->is_initialized()) {
        assets_->poll_file_changes();
    }
    if (engine == nullptr) {
        return;
    }
    if (shader_key_ != Key::Unknown && engine->window != nullptr && Input::state().key_pressed(shader_key_)) {
        shader_reload_requested_ = true;
    }
    if (shader_reload_requested_ && engine->renderer != nullptr) {
        shader_reload_requested_ = false;
        auto* r                  = static_cast<renderer::Renderer*>(engine->renderer);
        if (auto result = r->reload_shaders(); result) {
            ++shader_reloads_;
            AE_LOG_INFO("Renderer", "shaders reloaded");
        } else {
            AE_LOG_ERROR("Renderer", "shader reload failed (previous pipelines kept): {}", result.error().message);
        }
    }
}

} // namespace aether::gameplay
