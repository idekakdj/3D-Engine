// aether/gameplay/render_bridge.h — the asset->render bridge and the ECS->RenderScene builder.
//
// ADR-0001/0003: the renderer never reads the ECS or the asset system. This header holds the
// gameplay-side glue:
//   * RenderResourceCache — AssetId -> renderer handle cache. Meshes, materials and textures
//     are requested from the AssetManager asynchronously on first use and registered with the
//     Renderer once their data is ready (callers get nullptr / the default material until then).
//     Hot reload is automatic: when an asset's data version changes (AssetManager
//     poll_file_changes), update() re-uploads it. Built-in assets (procedural_mesh.h) and
//     runtime assets (add_mesh / add_material) resolve without the asset database.
//   * extract_render_scene() — builds the frame's RenderScene from a World: camera (override or
//     the primary CameraComponent), lights, and one RenderMeshInstance per submesh of every
//     visible MeshRendererComponent. Optional hooks supply interpolated physics poses and
//     skinning palettes, so this function stays independent of physics / animation.
//   * RenderBridgeSubsystem — the engine subsystem running both each frame (on_update polls the
//     cache, on_render fills RenderContext::scene).
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/geometry.h"
#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/subsystem.h"
#include "aether/core/types.h"
#include "aether/gameplay/application.h"
#include "aether/renderer/render_scene.h"
#include "aether/scene/entity.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace aether {
class World;
}
namespace aether::assets {
class AssetManager;
struct MeshData;
struct MaterialData;
} // namespace aether::assets
namespace aether::renderer {
class Renderer;
}

namespace aether::gameplay {

struct RenderCacheStats {
    usize meshes    = 0; // registered with the renderer
    usize materials = 0;
    usize textures  = 0;
    usize pending   = 0; // waiting for asset data
    usize failed    = 0; // failed to load (logged once each)
    usize reloads   = 0; // re-uploads after hot reload since creation
};

class RenderResourceCache {
public:
    // `assets` may be null (then only built-in and runtime assets resolve).
    RenderResourceCache(renderer::Renderer& renderer, assets::AssetManager* assets);
    ~RenderResourceCache(); // releases every handle it registered

    RenderResourceCache(const RenderResourceCache&)            = delete;
    RenderResourceCache& operator=(const RenderResourceCache&) = delete;

    struct Mesh {
        renderer::MeshHandle handle;
        std::vector<Submesh> submeshes;
        AABB                 bounds{};
        bool                 skinned = false;
    };

    // Starts loading on first use. nullptr while loading, after a failure, or for invalid ids.
    [[nodiscard]] const Mesh* mesh(const AssetId& id);
    // The default material while loading / on failure / for invalid ids. Materials whose
    // textures are still loading are registered without them and upgraded by update().
    [[nodiscard]] renderer::MaterialHandle material(const AssetId& id);
    // Invalid handle while loading / on failure.
    [[nodiscard]] renderer::TextureHandle texture(const AssetId& id);
    [[nodiscard]] renderer::MaterialHandle default_material();

    // Runtime assets: registered immediately under `id` (replacing a previous registration).
    void add_mesh(const AssetId& id, const assets::MeshData& data, std::string_view debug_name = {});
    void add_material(const AssetId& id, const assets::MaterialData& data);

    // Once per frame: finishes pending loads, upgrades materials whose textures became ready
    // and re-uploads assets whose data changed (hot reload).
    void update();
    // Releases the asset's renderer resources; the next request re-resolves it.
    void invalidate(const AssetId& id);
    // Releases everything.
    void clear();

    [[nodiscard]] RenderCacheStats    stats() const;
    [[nodiscard]] renderer::Renderer& renderer() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SceneExtractOptions {
    UVec2                         viewport{ 1280, 720 };
    std::optional<CameraOverride> camera; // otherwise the primary CameraComponent
    f32                           exposure = 1.0f;
    // Optional: world matrix to render `e` with (e.g. interpolated physics pose). Return false
    // to use the ECS world matrix.
    std::function<bool(Entity e, Mat4& out_world)> world_matrix;
    // Optional: skinning palette (joint model * inverse bind) for a skinned mesh on `e`.
    std::function<std::span<const Mat4>(Entity e)> skinning_palette;
};

struct SceneExtractStats {
    u32    instances      = 0;
    u32    lights         = 0;
    u32    skinned        = 0; // instances drawn with a palette
    u32    pending_meshes = 0; // mesh renderers whose mesh is not ready yet
    Entity camera         = kNullEntity; // ECS camera used (kNullEntity: override or default)
};

// Clears `out` and fills it from `world` (which should have up-to-date world matrices).
SceneExtractStats extract_render_scene(const World& world, RenderResourceCache& cache,
                                       const SceneExtractOptions& options, renderer::RenderScene& out);

// The first visible CameraComponent with primary == true (else the first visible camera).
[[nodiscard]] Entity find_primary_camera(const World& world);
// ADR-0016: the renderer's GI volume for a GIVolumeComponent with world matrix `world` (centre =
// translation, size = the lengths of the basis vectors; rotation ignored). Probe counts per axis =
// size / probe_spacing + 1, clamped to [2, 64]. enabled = false for a degenerate box.
struct GIVolumeComponent;
[[nodiscard]] renderer::GiVolume gi_volume_from(const Mat4& world, const GIVolumeComponent& volume);
// ADR-0017: the renderer's reflection probe for a ReflectionProbeComponent (capture point = the
// translation, box = translation +- half the basis lengths). box_min == box_max for a degenerate box.
struct ReflectionProbeComponent;
[[nodiscard]] renderer::ReflectionProbe reflection_probe_from(const Mat4& world, const ReflectionProbeComponent& probe,
                                                              u32 id);
// View parameters for a camera with world matrix `camera_world` (scale is ignored).
[[nodiscard]] renderer::RenderView make_render_view(const Mat4& camera_world, f32 fov_y_radians,
                                                    f32 near_z, f32 far_z, UVec2 viewport);
// World-space bounds of `local` transformed by `m` (exact for affine transforms).
[[nodiscard]] AABB transform_aabb(const Mat4& m, const AABB& local);

class RenderBridgeSubsystem final : public ISubsystem {
public:
    explicit RenderBridgeSubsystem(assets::AssetManager* assets = nullptr);
    ~RenderBridgeSubsystem() override;

    [[nodiscard]] const char* name() const override { return "RenderBridge"; }
    void on_startup(EngineContext& ctx) override;   // needs ctx.renderer
    void on_shutdown() override;
    void on_update(FrameContext& ctx) override;     // cache.update()
    void on_render(RenderContext& ctx) override;    // extract ctx.engine->world into ctx.scene

    // Null before startup / without a renderer.
    [[nodiscard]] RenderResourceCache* cache() noexcept { return cache_.get(); }

    // Per-frame inputs (the Application sets these before on_render).
    void set_viewport(UVec2 viewport) { options_.viewport = viewport; }
    void set_camera_override(std::optional<CameraOverride> camera) { options_.camera = std::move(camera); }
    void set_world_matrix_hook(std::function<bool(Entity, Mat4&)> hook) { options_.world_matrix = std::move(hook); }
    void set_palette_hook(std::function<std::span<const Mat4>(Entity)> hook) {
        options_.skinning_palette = std::move(hook);
    }
    // Runs after extraction (e.g. to append physics debug lines).
    void set_post_extract_hook(std::function<void(renderer::RenderScene&)> hook) { post_ = std::move(hook); }
    // Replaces the default environment (the procedural sky registered at startup, see below).
    void set_environment(const renderer::EnvironmentSettings& env) { environment_ = env; }
    [[nodiscard]] const renderer::EnvironmentSettings& environment() const noexcept { return environment_; }
    // When enabled (default) the subsystem registers a procedural HDR sky (environment.h) at
    // startup and uses it as skybox + IBL until set_environment() is called. Set before startup.
    void set_default_sky(bool enabled) { default_sky_enabled_ = enabled; }
    void set_exposure(f32 exposure) { options_.exposure = exposure; }

    [[nodiscard]] const SceneExtractStats& last_stats() const noexcept { return stats_; }

private:
    assets::AssetManager*                        assets_ = nullptr;
    EngineContext*                               engine_ = nullptr;
    std::unique_ptr<RenderResourceCache>         cache_;
    SceneExtractOptions                          options_;
    std::function<void(renderer::RenderScene&)> post_;
    renderer::EnvironmentSettings                environment_{};
    renderer::EnvHandle                          default_sky_;
    bool                                         default_sky_enabled_ = true;
    SceneExtractStats                            stats_{};
};

} // namespace aether::gameplay
