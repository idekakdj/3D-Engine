// animation_bridge.cpp — AnimationAssetBinder (see animation_bridge.h).
#include "aether/gameplay/animation_bridge.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/anim_graph.h"
#    include "aether/animation/clip.h"
#    include "aether/animation/components.h"
#    include "aether/animation/skeleton.h"
#endif

#include <unordered_map>
#include <unordered_set>

namespace aether::gameplay {

#if AE_WITH_ANIMATION

struct AnimationAssetBinder::Impl {
    struct SkeletonEntry {
        assets::AssetRef<assets::SkeletonData>        ref;
        u32                                           version = 0;
        std::shared_ptr<const animation::Skeleton>    skeleton;
        bool                                          failed = false;
    };
    struct GraphKey {
        AssetId skeleton;
        AssetId clip;
        bool    operator==(const GraphKey&) const = default;
    };
    struct GraphKeyHash {
        usize operator()(const GraphKey& k) const noexcept {
            return std::hash<AssetId>{}(k.skeleton) ^ (std::hash<AssetId>{}(k.clip) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct GraphEntry {
        assets::AssetRef<assets::AnimationClipData> ref;
        u32                                         version = 0;
        const animation::Skeleton*                  built_for = nullptr;
        std::shared_ptr<const animation::AnimGraph> graph;
        bool                                        failed = false;
    };

    assets::AssetManager*                                          assets = nullptr;
    std::unordered_map<AssetId, SkeletonEntry>                     skeletons;
    std::unordered_map<GraphKey, GraphEntry, GraphKeyHash>         graphs;

    [[nodiscard]] bool can_load() const { return assets != nullptr && assets->is_initialized(); }

    std::shared_ptr<const animation::Skeleton> skeleton(const AssetId& id, bool wait) {
        auto [it, inserted] = skeletons.try_emplace(id);
        SkeletonEntry& e    = it->second;
        if (inserted) {
            if (!can_load()) {
                e.failed = true;
                return nullptr;
            }
            e.ref = wait ? assets->load_sync<assets::SkeletonData>(id) : assets->load<assets::SkeletonData>(id);
        }
        if (e.ref.ready() && e.ref.version() != e.version) {
            e.version = e.ref.version();
            auto sk   = animation::Skeleton::create(*e.ref.get());
            if (!sk) {
                if (!e.failed) {
                    AE_LOG_ERROR("Animation", "skeleton {}: {}", id.to_string(), sk.error().message);
                }
                e.failed   = true;
                e.skeleton = nullptr;
            } else {
                e.failed   = false;
                e.skeleton = std::make_shared<const animation::Skeleton>(std::move(*sk));
            }
        } else if (e.ref.failed() && !e.failed) {
            e.failed = true;
            AE_LOG_ERROR("Animation", "skeleton {} failed to load: {}", id.to_string(), e.ref.error().message);
        }
        return e.skeleton;
    }

    std::shared_ptr<const animation::AnimGraph> graph(const std::shared_ptr<const animation::Skeleton>& sk,
                                                      const AssetId& skeleton_id, const AssetId& clip_id,
                                                      bool wait) {
        auto [it, inserted] = graphs.try_emplace(GraphKey{ skeleton_id, clip_id });
        GraphEntry& e       = it->second;
        if (inserted) {
            if (!can_load()) {
                e.failed = true;
                return nullptr;
            }
            e.ref = wait ? assets->load_sync<assets::AnimationClipData>(clip_id)
                         : assets->load<assets::AnimationClipData>(clip_id);
        }
        const bool stale = e.ref.ready() && (e.ref.version() != e.version || e.built_for != sk.get());
        if (stale) {
            e.version   = e.ref.version();
            e.built_for = sk.get();
            auto clip   = animation::AnimationClip::create(*e.ref.get(), *sk);
            if (!clip) {
                AE_LOG_ERROR("Animation", "clip {}: {}", clip_id.to_string(), clip.error().message);
                e.failed = true;
                e.graph  = nullptr;
                return nullptr;
            }
            auto g = animation::AnimGraph::make_single_clip(
                sk, std::make_shared<const animation::AnimationClip>(std::move(*clip)));
            if (!g) {
                AE_LOG_ERROR("Animation", "clip {}: {}", clip_id.to_string(), g.error().message);
                e.failed = true;
                e.graph  = nullptr;
                return nullptr;
            }
            e.failed = false;
            e.graph  = std::move(*g);
        } else if (e.ref.failed() && !e.failed) {
            e.failed = true;
            AE_LOG_ERROR("Animation", "clip {} failed to load: {}", clip_id.to_string(), e.ref.error().message);
        }
        return e.graph;
    }

    // Returns true when the animator is fully bound.
    bool bind(animation::AnimatorComponent& a, bool wait) {
        if (!a.skeleton_asset.is_valid()) {
            return a.skeleton() != nullptr; // runtime-only animator: nothing to resolve
        }
        const auto sk = skeleton(a.skeleton_asset, wait);
        if (!sk) {
            return false;
        }
        if (a.skeleton() != sk) {
            a.set_skeleton(sk);
        }
        if (!a.graph_asset.is_valid()) {
            return true;
        }
        const auto g = graph(sk, a.skeleton_asset, a.graph_asset, wait);
        if (!g) {
            return false;
        }
        if (!a.graph_instance().valid() || a.graph_instance().graph() != g) {
            a.set_graph(g);
        }
        return true;
    }
};

AnimationAssetBinder::AnimationAssetBinder(assets::AssetManager* assets) : impl_(std::make_unique<Impl>()) {
    impl_->assets = assets;
}

void AnimationAssetBinder::bind_world(World& world, bool wait) {
    u32 bound = 0;
    for (const auto [e, anim] : world.registry().view<animation::AnimatorComponent>().each()) {
        (void)e;
        bound += impl_->bind(anim, wait) ? 1u : 0u;
    }
    bound_ = bound;
}

#else // !AE_WITH_ANIMATION

struct AnimationAssetBinder::Impl {};

AnimationAssetBinder::AnimationAssetBinder(assets::AssetManager*) : impl_(std::make_unique<Impl>()) {}

void AnimationAssetBinder::bind_world(World&, bool) { bound_ = 0; }

#endif

AnimationAssetBinder::~AnimationAssetBinder() = default;

void AnimationAssetBinder::on_startup(EngineContext& ctx) { engine_ = &ctx; }

void AnimationAssetBinder::on_shutdown() { engine_ = nullptr; }

void AnimationAssetBinder::on_begin_frame(FrameContext& ctx) {
    EngineContext* engine = ctx.engine != nullptr ? ctx.engine : engine_;
    if (engine != nullptr && engine->world != nullptr) {
        bind_world(*engine->world, false);
    }
}

} // namespace aether::gameplay
