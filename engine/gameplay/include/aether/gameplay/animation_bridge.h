// aether/gameplay/animation_bridge.h — resolves animator asset ids into runtime animation data.
//
// AnimatorComponent stores serializable AssetIds (skeleton_asset, graph_asset) next to its
// runtime Skeleton / AnimGraph. AnimationAssetBinder (SubsystemKind::Engine, on_begin_frame)
// finds animators whose runtime objects are missing or stale and binds them:
//   * skeleton_asset -> assets::SkeletonData -> animation::Skeleton (shared per id);
//   * graph_asset    -> an assets::AnimationClipData, played looping as a single-clip graph
//                       (AnimGraph assets do not exist yet; hand-built graphs set via
//                       AnimatorComponent::set_graph are left alone).
// Loads are asynchronous; an animator stays in its bind pose until its data is ready. Built
// objects are cached per id and rebuilt when the asset's data version changes (hot reload).
// Without the animation module (AE_WITH_ANIMATION=0) the subsystem does nothing.
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/subsystem.h"
#include "aether/core/types.h"

#include <memory>

namespace aether {
class World;
}
namespace aether::assets {
class AssetManager;
}

namespace aether::gameplay {

class AnimationAssetBinder final : public ISubsystem {
public:
    explicit AnimationAssetBinder(assets::AssetManager* assets);
    ~AnimationAssetBinder() override;

    [[nodiscard]] const char* name() const override { return "AnimationAssetBinder"; }
    void on_startup(EngineContext& ctx) override;
    void on_shutdown() override;
    void on_begin_frame(FrameContext& ctx) override;

    // Binds every resolvable animator of `world` now (tools/tests; `wait` blocks on the loads).
    void bind_world(World& world, bool wait = false);

    [[nodiscard]] u32 bound_count() const noexcept { return bound_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EngineContext*        engine_ = nullptr;
    u32                   bound_  = 0;
};

} // namespace aether::gameplay
