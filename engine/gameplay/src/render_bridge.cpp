// render_bridge.cpp — RenderResourceCache (asset -> renderer handles), ECS -> RenderScene
// extraction and the RenderBridgeSubsystem.
#include "aether/gameplay/render_bridge.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/renderer/instance_flags.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace aether::gameplay {

namespace {

enum class LoadState : u8 { Pending = 0, Ready, Failed };

rhi::Format to_rhi_format(assets::TextureFormat f) {
    switch (f) {
    case assets::TextureFormat::RGBA8_UNORM: return rhi::Format::RGBA8Unorm;
    case assets::TextureFormat::RGBA8_SRGB: return rhi::Format::RGBA8Srgb;
    case assets::TextureFormat::RGBA16F: return rhi::Format::RGBA16F;
    case assets::TextureFormat::RGBA32F: return rhi::Format::RGBA32F;
    }
    return rhi::Format::RGBA8Srgb;
}

usize texel_bytes(assets::TextureFormat f) {
    switch (f) {
    case assets::TextureFormat::RGBA16F: return 8;
    case assets::TextureFormat::RGBA32F: return 16;
    default: return 4;
    }
}

renderer::BlendMode to_blend(assets::AlphaMode m) {
    switch (m) {
    case assets::AlphaMode::Mask: return renderer::BlendMode::Masked;
    case assets::AlphaMode::Blend: return renderer::BlendMode::Translucent;
    case assets::AlphaMode::Opaque: break;
    }
    return renderer::BlendMode::Opaque;
}

std::string debug_name_for(const AssetId& id, std::string_view kind) {
    return std::string(kind) + ":" + id.to_string().substr(0, 8);
}

} // namespace

// =================================================================================================
// RenderResourceCache::Impl
// =================================================================================================
struct RenderResourceCache::Impl {
    struct MeshEntry {
        LoadState                          state = LoadState::Pending;
        Mesh                               mesh;
        assets::AssetRef<assets::MeshData> ref;
        u32                                version = 0; // data version that was uploaded
        bool                               runtime = false;
    };
    struct TextureEntry {
        LoadState                             state = LoadState::Pending;
        renderer::TextureHandle               handle;
        assets::AssetRef<assets::TextureData> ref;
        u32                                   version = 0;
    };
    struct MaterialEntry {
        LoadState                              state = LoadState::Pending;
        renderer::MaterialHandle               handle;
        assets::AssetRef<assets::MaterialData> ref;
        u32                                    version = 0;
        bool                                   complete = false; // every referenced texture resolved
        bool                                   runtime  = false;
        assets::MaterialData                   data;             // last uploaded description
    };

    renderer::Renderer&    renderer;
    assets::AssetManager*  assets;
    renderer::MaterialHandle default_material;

    std::unordered_map<AssetId, MeshEntry>     meshes;
    std::unordered_map<AssetId, TextureEntry>  textures;
    std::unordered_map<AssetId, MaterialEntry> materials;
    usize                                      reloads = 0;

    Impl(renderer::Renderer& r, assets::AssetManager* a) : renderer(r), assets(a) {}

    [[nodiscard]] bool can_load() const { return assets != nullptr && assets->is_initialized(); }

    // ---- meshes --------------------------------------------------------------------------------
    void upload_mesh(MeshEntry& e, const assets::MeshData& data, std::string debug_name) {
        renderer::MeshUpload up;
        up.vertices   = data.vertices;
        up.skin       = data.skin;
        up.indices    = data.indices;
        up.submeshes  = data.submeshes;
        up.bounds     = data.bounds;
        up.debug_name = std::move(debug_name);
        const renderer::MeshHandle h = renderer.register_mesh(up);
        if (e.mesh.handle.is_valid()) {
            renderer.release(e.mesh.handle);
        }
        e.mesh.handle    = h;
        e.mesh.submeshes = data.submeshes;
        if (e.mesh.submeshes.empty()) {
            Submesh all;
            all.index_count = static_cast<u32>(data.indices.size());
            all.bounds      = data.bounds;
            e.mesh.submeshes.push_back(all);
        }
        e.mesh.bounds  = data.bounds;
        e.mesh.skinned = !data.skin.empty();
        e.state        = h.is_valid() ? LoadState::Ready : LoadState::Failed;
    }

    void poll_mesh(const AssetId& id, MeshEntry& e) {
        if (e.runtime || !e.ref.valid()) {
            return;
        }
        if (e.ref.ready() && e.ref.version() != e.version) {
            if (e.version != 0) {
                ++reloads;
            }
            e.version = e.ref.version();
            upload_mesh(e, *e.ref.get(), debug_name_for(id, "mesh"));
        } else if (e.ref.failed() && e.state == LoadState::Pending) {
            e.state = LoadState::Failed;
            AE_LOG_ERROR("RenderBridge", "mesh {} failed to load: {}", id.to_string(), e.ref.error().message);
        }
    }

    MeshEntry* find_or_start_mesh(const AssetId& id) {
        if (!id.is_valid()) {
            return nullptr;
        }
        if (const auto it = meshes.find(id); it != meshes.end()) {
            if (it->second.state == LoadState::Pending) {
                poll_mesh(id, it->second);
            }
            return &it->second;
        }
        MeshEntry& e = meshes[id];
        if (const auto builtin = builtin_mesh_from_id(id)) {
            e.runtime = true;
            upload_mesh(e, make_builtin_mesh(*builtin), debug_name_for(id, "builtin_mesh"));
            return &e;
        }
        if (!can_load()) {
            e.state = LoadState::Failed;
            AE_LOG_WARN("RenderBridge", "mesh {} requested without an asset manager", id.to_string());
            return &e;
        }
        e.ref = assets->load<assets::MeshData>(id);
        poll_mesh(id, e);
        return &e;
    }

    // ---- textures ------------------------------------------------------------------------------
    void upload_texture(const AssetId& id, TextureEntry& e, const assets::TextureData& data) {
        renderer::TextureUpload up;
        up.format        = to_rhi_format(data.format);
        up.width         = data.width;
        up.height        = data.height;
        up.array_layers  = std::max(data.array_layers, 1u);
        up.cubemap       = data.is_cubemap;
        up.generate_mips = true;
        // Only mip 0 is uploaded (the renderer regenerates the chain).
        const usize mip0 = static_cast<usize>(data.width) * data.height * up.array_layers * texel_bytes(data.format);
        if (data.width == 0 || data.height == 0 || data.pixels.size() < mip0) {
            AE_LOG_ERROR("RenderBridge", "texture {} has no usable pixel data", id.to_string());
            e.state = LoadState::Failed;
            return;
        }
        up.pixels     = ByteSpan(reinterpret_cast<const byte*>(data.pixels.data()), mip0);
        up.debug_name = debug_name_for(id, "texture");
        const renderer::TextureHandle h = renderer.register_texture(up);
        if (e.handle.is_valid()) {
            renderer.release(e.handle);
        }
        e.handle = h;
        e.state  = h.is_valid() ? LoadState::Ready : LoadState::Failed;
    }

    // Returns true when the texture changed (materials using it must be refreshed).
    bool poll_texture(const AssetId& id, TextureEntry& e) {
        if (!e.ref.valid()) {
            return false;
        }
        if (e.ref.ready() && e.ref.version() != e.version) {
            if (e.version != 0) {
                ++reloads;
            }
            e.version = e.ref.version();
            upload_texture(id, e, *e.ref.get());
            return true;
        }
        if (e.ref.failed() && e.state == LoadState::Pending) {
            e.state = LoadState::Failed;
            AE_LOG_ERROR("RenderBridge", "texture {} failed to load: {}", id.to_string(), e.ref.error().message);
            return true;
        }
        return false;
    }

    TextureEntry* find_or_start_texture(const AssetId& id) {
        if (!id.is_valid()) {
            return nullptr;
        }
        if (const auto it = textures.find(id); it != textures.end()) {
            if (it->second.state == LoadState::Pending) {
                poll_texture(id, it->second);
            }
            return &it->second;
        }
        TextureEntry& e = textures[id];
        if (!can_load()) {
            e.state = LoadState::Failed;
            return &e;
        }
        e.ref = assets->load<assets::TextureData>(id);
        poll_texture(id, e);
        return &e;
    }

    // ---- materials -----------------------------------------------------------------------------
    // Resolves the material's textures (starting their loads) and fills `desc`. Returns whether
    // every referenced texture has settled (ready or failed).
    bool build_material(const assets::MaterialData& d, renderer::MaterialDesc& desc) {
        desc.base_color         = d.base_color_factor;
        desc.emissive           = d.emissive_factor;
        desc.metallic           = d.metallic_factor;
        desc.roughness          = d.roughness_factor;
        desc.normal_scale       = d.normal_scale;
        desc.occlusion_strength = d.occlusion_strength;
        desc.alpha_cutoff       = d.alpha_cutoff;
        desc.blend              = to_blend(d.alpha_mode);
        desc.double_sided       = d.double_sided;
        desc.debug_name         = d.name;
        bool complete           = true;
        const auto resolve      = [&](const AssetId& tid) -> renderer::TextureHandle {
            TextureEntry* t = find_or_start_texture(tid);
            if (t == nullptr) {
                return {};
            }
            if (t->state == LoadState::Pending) {
                complete = false;
            }
            return t->state == LoadState::Ready ? t->handle : renderer::TextureHandle{};
        };
        desc.base_color_tex         = resolve(d.base_color_texture);
        desc.metallic_roughness_tex = resolve(d.metallic_roughness_texture);
        desc.normal_tex             = resolve(d.normal_texture);
        desc.occlusion_tex          = resolve(d.occlusion_texture);
        desc.emissive_tex           = resolve(d.emissive_texture);
        return complete;
    }

    void upload_material(MaterialEntry& e, const assets::MaterialData& d) {
        renderer::MaterialDesc desc;
        e.complete = build_material(d, desc);
        e.data     = d;
        if (e.handle.is_valid()) {
            renderer.update_material(e.handle, desc);
        } else {
            e.handle = renderer.register_material(desc);
        }
        e.state = e.handle.is_valid() ? LoadState::Ready : LoadState::Failed;
    }

    void poll_material(const AssetId& id, MaterialEntry& e) {
        if (!e.runtime && e.ref.valid()) {
            if (e.ref.ready() && e.ref.version() != e.version) {
                if (e.version != 0) {
                    ++reloads;
                }
                e.version = e.ref.version();
                upload_material(e, *e.ref.get());
                return;
            }
            if (e.ref.failed() && e.state == LoadState::Pending) {
                e.state = LoadState::Failed;
                AE_LOG_ERROR("RenderBridge", "material {} failed to load: {}", id.to_string(),
                             e.ref.error().message);
                return;
            }
        }
        if (e.state == LoadState::Ready && !e.complete) {
            renderer::MaterialDesc desc;
            if (build_material(e.data, desc)) {
                e.complete = true;
                renderer.update_material(e.handle, desc);
            }
        }
    }

    MaterialEntry* find_or_start_material(const AssetId& id) {
        if (!id.is_valid()) {
            return nullptr;
        }
        if (const auto it = materials.find(id); it != materials.end()) {
            if (it->second.state == LoadState::Pending) {
                poll_material(id, it->second);
            }
            return &it->second;
        }
        MaterialEntry& e = materials[id];
        if (id == default_material_id()) {
            e.runtime = true;
            upload_material(e, make_default_material());
            return &e;
        }
        if (!can_load()) {
            e.state = LoadState::Failed;
            return &e;
        }
        e.ref = assets->load<assets::MaterialData>(id);
        poll_material(id, e);
        return &e;
    }

    renderer::MaterialHandle fallback() {
        if (!default_material.is_valid()) {
            renderer::MaterialDesc desc;
            build_material(make_default_material(), desc);
            default_material = renderer.register_material(desc);
        }
        return default_material;
    }

    // Materials that reference `tex` get their texture slots rebuilt.
    void refresh_materials_using(const AssetId& tex) {
        for (auto& [id, m] : materials) {
            const assets::MaterialData& d = m.data;
            if (m.state == LoadState::Ready &&
                (d.base_color_texture == tex || d.metallic_roughness_texture == tex || d.normal_texture == tex ||
                 d.occlusion_texture == tex || d.emissive_texture == tex)) {
                m.complete = false;
                renderer::MaterialDesc desc;
                m.complete = build_material(d, desc);
                renderer.update_material(m.handle, desc);
            }
        }
    }

    void release_all() {
        for (auto& [id, m] : meshes) {
            if (m.mesh.handle.is_valid()) {
                renderer.release(m.mesh.handle);
            }
        }
        for (auto& [id, m] : materials) {
            if (m.handle.is_valid()) {
                renderer.release(m.handle);
            }
        }
        for (auto& [id, t] : textures) {
            if (t.handle.is_valid()) {
                renderer.release(t.handle);
            }
        }
        if (default_material.is_valid()) {
            renderer.release(default_material);
        }
        meshes.clear();
        materials.clear();
        textures.clear();
        default_material = {};
    }
};

// =================================================================================================
// RenderResourceCache
// =================================================================================================
RenderResourceCache::RenderResourceCache(renderer::Renderer& renderer, assets::AssetManager* assets)
    : impl_(std::make_unique<Impl>(renderer, assets)) {}

RenderResourceCache::~RenderResourceCache() { impl_->release_all(); }

const RenderResourceCache::Mesh* RenderResourceCache::mesh(const AssetId& id) {
    const Impl::MeshEntry* e = impl_->find_or_start_mesh(id);
    return e != nullptr && e->state == LoadState::Ready ? &e->mesh : nullptr;
}

renderer::MaterialHandle RenderResourceCache::material(const AssetId& id) {
    const Impl::MaterialEntry* e = impl_->find_or_start_material(id);
    return e != nullptr && e->state == LoadState::Ready ? e->handle : impl_->fallback();
}

renderer::TextureHandle RenderResourceCache::texture(const AssetId& id) {
    const Impl::TextureEntry* e = impl_->find_or_start_texture(id);
    return e != nullptr && e->state == LoadState::Ready ? e->handle : renderer::TextureHandle{};
}

renderer::MaterialHandle RenderResourceCache::default_material() { return impl_->fallback(); }

void RenderResourceCache::add_mesh(const AssetId& id, const assets::MeshData& data, std::string_view debug_name) {
    if (!id.is_valid()) {
        return;
    }
    Impl::MeshEntry& e = impl_->meshes[id];
    e.runtime          = true;
    e.ref              = {};
    impl_->upload_mesh(e, data, debug_name.empty() ? debug_name_for(id, "runtime_mesh") : std::string(debug_name));
}

void RenderResourceCache::add_material(const AssetId& id, const assets::MaterialData& data) {
    if (!id.is_valid()) {
        return;
    }
    Impl::MaterialEntry& e = impl_->materials[id];
    e.runtime              = true;
    e.ref                  = {};
    impl_->upload_material(e, data);
}

void RenderResourceCache::update() {
    std::vector<AssetId> changed_textures;
    for (auto& [id, t] : impl_->textures) {
        if (impl_->poll_texture(id, t)) {
            changed_textures.push_back(id);
        }
    }
    for (const AssetId& id : changed_textures) {
        impl_->refresh_materials_using(id);
    }
    for (auto& [id, m] : impl_->meshes) {
        impl_->poll_mesh(id, m);
    }
    for (auto& [id, m] : impl_->materials) {
        impl_->poll_material(id, m);
    }
}

void RenderResourceCache::invalidate(const AssetId& id) {
    if (const auto it = impl_->meshes.find(id); it != impl_->meshes.end()) {
        if (it->second.mesh.handle.is_valid()) {
            impl_->renderer.release(it->second.mesh.handle);
        }
        impl_->meshes.erase(it);
    }
    if (const auto it = impl_->materials.find(id); it != impl_->materials.end()) {
        if (it->second.handle.is_valid()) {
            impl_->renderer.release(it->second.handle);
        }
        impl_->materials.erase(it);
    }
    if (const auto it = impl_->textures.find(id); it != impl_->textures.end()) {
        const renderer::TextureHandle old = it->second.handle;
        impl_->textures.erase(it);
        impl_->refresh_materials_using(id); // restarts the texture load, drops the stale handle
        if (old.is_valid()) {
            impl_->renderer.release(old);
        }
    }
}

void RenderResourceCache::clear() { impl_->release_all(); }

RenderCacheStats RenderResourceCache::stats() const {
    RenderCacheStats s;
    s.reloads         = impl_->reloads;
    const auto count  = [&](LoadState state, usize& ready) {
        switch (state) {
        case LoadState::Ready: ++ready; break;
        case LoadState::Pending: ++s.pending; break;
        case LoadState::Failed: ++s.failed; break;
        }
    };
    for (const auto& [id, e] : impl_->meshes) {
        count(e.state, s.meshes);
    }
    for (const auto& [id, e] : impl_->materials) {
        count(e.state, s.materials);
    }
    for (const auto& [id, e] : impl_->textures) {
        count(e.state, s.textures);
    }
    return s;
}

renderer::Renderer& RenderResourceCache::renderer() noexcept { return impl_->renderer; }

// =================================================================================================
// extraction
// =================================================================================================
AABB transform_aabb(const Mat4& m, const AABB& local) {
    const Vec3 c = local.center();
    const Vec3 e = local.extent();
    const Vec3 wc(m * Vec4(c, 1.0f));
    Vec3       we(0.0f);
    for (int col = 0; col < 3; ++col) {
        we += glm::abs(Vec3(m[col])) * e[col];
    }
    AABB out;
    out.min = wc - we;
    out.max = wc + we;
    return out;
}

Entity find_primary_camera(const World& world) {
    // Ties resolve to the oldest entity (lowest id) so the choice does not depend on EnTT's
    // storage iteration order.
    Entity primary  = kNullEntity;
    Entity fallback = kNullEntity;
    const auto older = [](Entity a, Entity b) {
        return b == kNullEntity || entt::to_integral(a) < entt::to_integral(b);
    };
    for (const auto [e, cam] : world.registry().view<const CameraComponent>().each()) {
        if (!scene::is_visible_in_hierarchy(world, e)) {
            continue;
        }
        if (cam.primary && older(e, primary)) {
            primary = e;
        }
        if (older(e, fallback)) {
            fallback = e;
        }
    }
    return primary != kNullEntity ? primary : fallback;
}

renderer::RenderView make_render_view(const Mat4& camera_world, f32 fov_y_radians, f32 near_z, f32 far_z,
                                      UVec2 viewport) {
    // Rigid part of the camera transform (normalised axes; scale must not skew the view).
    Mat4 rigid(1.0f);
    for (int c = 0; c < 3; ++c) {
        const Vec3 axis = Vec3(camera_world[c]);
        const f32  len  = glm::length(axis);
        rigid[c]        = Vec4(len > 0.0f ? axis / len : Vec3(c == 0, c == 1, c == 2), 0.0f);
    }
    rigid[3] = Vec4(Vec3(camera_world[3]), 1.0f);

    renderer::RenderView v;
    const f32            aspect = static_cast<f32>(std::max(viewport.x, 1u)) / static_cast<f32>(std::max(viewport.y, 1u));
    v.view            = glm::inverse(rigid);
    v.proj            = perspective(fov_y_radians, aspect, near_z, far_z);
    v.view_proj       = v.proj * v.view;
    v.camera_position = Vec3(rigid[3]);
    v.near_z          = near_z;
    v.far_z           = far_z;
    v.viewport        = viewport;
    return v;
}

SceneExtractStats extract_render_scene(const World& world, RenderResourceCache& cache,
                                       const SceneExtractOptions& options, renderer::RenderScene& out) {
    SceneExtractStats stats;
    out.clear();
    const auto& reg = world.registry();

    const auto world_matrix = [&](Entity e) -> Mat4 {
        Mat4 m(1.0f);
        if (options.world_matrix && options.world_matrix(e, m)) {
            return m;
        }
        const auto* tc = reg.try_get<TransformComponent>(e);
        return tc != nullptr && !tc->dirty ? tc->world : world.world_matrix(e);
    };

    // ---- camera ------------------------------------------------------------------------------
    if (options.camera) {
        const CameraOverride& c = *options.camera;
        out.view                = make_render_view(glm::inverse(c.view), c.fov_y_radians, c.near_z, c.far_z,
                                                   options.viewport);
    } else if (const Entity cam = find_primary_camera(world); cam != kNullEntity) {
        const CameraComponent& cc = reg.get<CameraComponent>(cam);
        out.view     = make_render_view(world_matrix(cam), cc.fov_y_deg * kDeg2Rad, cc.near_z, cc.far_z,
                                        options.viewport);
        stats.camera = cam;
    } else {
        const Mat4 fallback = glm::inverse(look_at(Vec3(0, 2, 6), Vec3(0), Vec3(0, 1, 0)));
        out.view            = make_render_view(fallback, 60.0f * kDeg2Rad, 0.1f, 1000.0f, options.viewport);
    }
    out.view.exposure = options.exposure;

    // ---- lights ------------------------------------------------------------------------------
    for (const auto [e, lc] : reg.view<const LightComponent>().each()) {
        if (!scene::is_visible_in_hierarchy(world, e)) {
            continue;
        }
        const Mat4           m = world_matrix(e);
        renderer::RenderLight l;
        switch (lc.kind) {
        case LightKind::Directional: l.type = renderer::LightType::Directional; break;
        case LightKind::Point: l.type = renderer::LightType::Point; break;
        case LightKind::Spot: l.type = renderer::LightType::Spot; break;
        }
        l.position     = Vec3(m[3]);
        const Vec3 fwd = -Vec3(m[2]);
        l.direction    = glm::length(fwd) > 0.0f ? glm::normalize(fwd) : Vec3(0, -1, 0);
        l.color        = lc.color;
        l.intensity    = lc.intensity;
        l.range        = lc.range;
        const f32 inner = std::clamp(lc.inner_cone_deg, 0.0f, 89.9f);
        const f32 outer = std::clamp(std::max(lc.outer_cone_deg, inner), 0.0f, 89.9f);
        l.inner_cone   = std::cos(inner * kDeg2Rad);
        l.outer_cone   = std::cos(outer * kDeg2Rad);
        l.cast_shadows = lc.cast_shadows;
        out.lights.push_back(l);
        ++stats.lights;
    }

    // ---- meshes ------------------------------------------------------------------------------
    for (const auto [e, mr] : reg.view<const MeshRendererComponent>().each()) {
        if (!scene::is_visible_in_hierarchy(world, e)) {
            continue;
        }
        const RenderResourceCache::Mesh* mesh = cache.mesh(mr.mesh);
        if (mesh == nullptr) {
            ++stats.pending_meshes;
            continue;
        }
        const Mat4  m         = world_matrix(e);
        const auto* overrides = reg.try_get<gameplay::MaterialOverridesComponent>(e);

        u32 first_joint = kInvalidU32;
        u32 joint_count = 0;
        if (mesh->skinned && options.skinning_palette) {
            const std::span<const Mat4> palette = options.skinning_palette(e);
            if (!palette.empty()) {
                first_joint = static_cast<u32>(out.joint_matrices.size());
                joint_count = static_cast<u32>(palette.size());
                out.joint_matrices.insert(out.joint_matrices.end(), palette.begin(), palette.end());
            }
        }

        u32 flags = mr.cast_shadows ? renderer::instance_flags::kCastShadow : 0u;
        if (joint_count > 0) {
            flags |= renderer::instance_flags::kSkinned | renderer::instance_flags::kNeverCull;
            ++stats.skinned;
        }
        for (u32 i = 0; i < mesh->submeshes.size(); ++i) {
            const Submesh& sm       = mesh->submeshes[i];
            AssetId        material = overrides != nullptr ? overrides->slot(sm.material_slot) : AssetId{};
            if (!material.is_valid()) {
                material = mr.material;
            }
            renderer::RenderMeshInstance inst;
            inst.mesh         = mesh->handle;
            inst.material     = cache.material(material);
            inst.submesh      = i;
            inst.transform    = m;
            inst.world_bounds = transform_aabb(m, sm.bounds.valid() && sm.index_count > 0 &&
                                                          (sm.bounds.max != sm.bounds.min)
                                                      ? sm.bounds
                                                      : mesh->bounds);
            inst.flags        = flags;
            inst.first_joint  = first_joint;
            inst.joint_count  = joint_count;
            out.instances.push_back(inst);
            ++stats.instances;
        }
    }
    return stats;
}

// =================================================================================================
// RenderBridgeSubsystem
// =================================================================================================
RenderBridgeSubsystem::RenderBridgeSubsystem(assets::AssetManager* assets) : assets_(assets) {}

RenderBridgeSubsystem::~RenderBridgeSubsystem() = default;

void RenderBridgeSubsystem::on_startup(EngineContext& ctx) {
    engine_ = &ctx;
    if (!cache_ && ctx.renderer != nullptr) {
        cache_ = std::make_unique<RenderResourceCache>(*static_cast<renderer::Renderer*>(ctx.renderer), assets_);
    }
}

void RenderBridgeSubsystem::on_shutdown() {
    cache_.reset();
    engine_ = nullptr;
}

void RenderBridgeSubsystem::on_update(FrameContext&) {
    if (cache_) {
        cache_->update();
    }
}

void RenderBridgeSubsystem::on_render(RenderContext& ctx) {
    EngineContext* engine = ctx.engine != nullptr ? ctx.engine : engine_;
    if (!cache_ || ctx.scene == nullptr || engine == nullptr || engine->world == nullptr) {
        return;
    }
    stats_                  = extract_render_scene(*engine->world, *cache_, options_, *ctx.scene);
    ctx.scene->environment  = environment_;
    if (post_) {
        post_(*ctx.scene);
    }
}

} // namespace aether::gameplay
