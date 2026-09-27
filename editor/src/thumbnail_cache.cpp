// thumbnail_cache.cpp — see thumbnail_cache.h.
#include "thumbnail_cache.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/editor/prefab.h"
#include "aether/gameplay/component_codecs.h"
#include "aether/gameplay/environment.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/gameplay/scene_instantiation.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/command_list.h"
#include "aether/rhi/device.h"
#include "aether/rhi/imgui.h"
#include "aether/scene/components.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

// Decoding: stb_image's implementation lives in aether.assets (image_importer.cpp, STBI_NO_STDIO).
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace aether::editor {

namespace {
constexpr const char* kLog = "Thumbnails";
constexpr f32         kFovY = 0.7f; // ~40 degrees: little perspective distortion

std::filesystem::file_time_type mtime_of(const std::filesystem::path& p) {
    std::error_code ec;
    const auto      t = std::filesystem::last_write_time(p, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}
} // namespace

ThumbnailCache::ThumbnailCache(rhi::Device& device, assets::AssetManager& assets, std::filesystem::path content_root,
                               rhi::SamplerHandle sampler, bool shadows)
    : device_(device), assets_(assets), root_(std::move(content_root)), sampler_(sampler), shadows_(shadows) {}

ThumbnailCache::~ThumbnailCache() { shutdown(); }

// =================================================================================================
// queries
// =================================================================================================
u64 ThumbnailCache::get(const std::filesystem::path& rel) {
    const std::string key = rel.generic_string();
    auto              it  = entries_.find(key);
    if (it == entries_.end()) {
        Entry e;
        e.kind = thumbnail_kind(rel);
        if (e.kind != ThumbnailKind::None) {
            e.state = State::Pending;
            e.mtime = mtime_of(root_ / rel);
            (thumbnail_is_rendered(e.kind) ? render_queue_ : image_queue_).push_back(key);
        }
        it = entries_.emplace(key, std::move(e)).first;
    }
    return it->second.state == State::Ready ? it->second.imgui_id : 0u;
}

ThumbnailCache::State ThumbnailCache::state(const std::filesystem::path& rel) const {
    const auto it = entries_.find(rel.generic_string());
    return it == entries_.end() ? State::None : it->second.state;
}

rhi::TextureHandle ThumbnailCache::texture(const std::filesystem::path& rel) const {
    const auto it = entries_.find(rel.generic_string());
    return it != entries_.end() && it->second.state == State::Ready ? it->second.texture : rhi::TextureHandle{};
}

ThumbnailCache::Stats ThumbnailCache::stats() const {
    Stats s = counters_;
    for (const auto& [key, e] : entries_) {
        (void)key;
        s.ready += e.state == State::Ready ? 1u : 0u;
        s.pending += e.state == State::Pending ? 1u : 0u;
        s.failed += e.state == State::Failed ? 1u : 0u;
    }
    return s;
}

// =================================================================================================
// per frame
// =================================================================================================
void ThumbnailCache::update(u64 frame_index) {
    frame_ = frame_index;
    // Free textures ImGui / the GPU can no longer reference.
    const u64 delay = device_.frames_in_flight() + 1;
    std::erase_if(retired_, [&](const Retired& r) {
        if (r.frame + delay > frame_) {
            return false;
        }
        if (r.imgui_id != 0) {
            rhi::imgui_remove_texture(device_, r.imgui_id);
        }
        if (r.texture.is_valid()) {
            device_.destroy(r.texture);
        }
        return true;
    });
    if (frame_ % 60 == 0) {
        check_file_changes();
    }

    for (u32 n = 0; n < kImagesPerFrame && !image_queue_.empty(); ++n) {
        const std::string key = image_queue_.front();
        image_queue_.pop_front();
        if (auto it = entries_.find(key); it != entries_.end() && it->second.state == State::Pending) {
            decode_image(key, it->second);
        }
    }

    if (job_.world == nullptr) {
        while (!render_queue_.empty() && job_.world == nullptr) {
            const std::string key = render_queue_.front();
            render_queue_.pop_front();
            if (auto it = entries_.find(key); it != entries_.end() && it->second.state == State::Pending) {
                start_job(key);
            }
        }
    } else {
        advance_job();
    }
}

void ThumbnailCache::render(rhi::CommandList& cmd) {
    if (job_.world == nullptr || !job_.ready || !renderer_) {
        return;
    }
    auto it = entries_.find(job_.key);
    if (it == entries_.end()) {
        finish_job(false, "entry vanished");
        return;
    }
    const rhi::TextureHandle target_tex = create_texture(true, "Thumbnail.Render");
    if (!target_tex.is_valid()) {
        finish_job(false, "cannot create the render target");
        return;
    }
    renderer::RenderTarget t;
    t.texture       = target_tex;
    t.format        = rhi::Format::RGBA8Unorm;
    t.extent        = UVec2(kSize);
    t.initial_state = rhi::ResourceState::Undefined;
    t.final_state   = rhi::ResourceState::ShaderRead; // sampled by ImGui
    cmd.push_debug_group("Thumbnail");
    renderer_->render(job_.scene, cmd, t);
    cmd.pop_debug_group();
    publish(it->second, target_tex);
    ++counters_.rendered;
    finish_job(true);
}

void ThumbnailCache::clear() {
    for (auto& [key, e] : entries_) {
        (void)key;
        retire(e);
    }
    entries_.clear();
    image_queue_.clear();
    render_queue_.clear();
    job_ = Job{};
}

void ThumbnailCache::shutdown() {
    job_ = Job{};
    for (auto& [key, e] : entries_) {
        (void)key;
        retire(e);
    }
    entries_.clear();
    for (const Retired& r : retired_) {
        if (r.imgui_id != 0) {
            rhi::imgui_remove_texture(device_, r.imgui_id);
        }
        if (r.texture.is_valid()) {
            device_.destroy(r.texture);
        }
    }
    retired_.clear();
    cache_.reset(); // releases its renderer resources first
    if (renderer_ && sky_.is_valid()) {
        renderer_->release(sky_);
    }
    sky_ = {};
    renderer_.reset();
}

// =================================================================================================
// images (CPU)
// =================================================================================================
void ThumbnailCache::decode_image(const std::string& key, Entry& e) {
    const std::vector<byte> bytes = paths::read_file(root_ / key);
    const auto*             data  = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int               size  = static_cast<int>(bytes.size());
    int                     w = 0, h = 0, n = 0;
    ThumbnailImage          img;
    if (!bytes.empty() && stbi_is_hdr_from_memory(data, size)) {
        if (f32* px = stbi_loadf_from_memory(data, size, &w, &h, &n, 4)) {
            const std::vector<u8> ldr = hdr_to_display(px, static_cast<u32>(w), static_cast<u32>(h));
            stbi_image_free(px);
            img = fit_thumbnail(ldr.data(), static_cast<u32>(w), static_cast<u32>(h), kSize);
        }
    } else if (!bytes.empty()) {
        if (stbi_uc* px = stbi_load_from_memory(data, size, &w, &h, &n, 4)) {
            img = fit_thumbnail(px, static_cast<u32>(w), static_cast<u32>(h), kSize);
            stbi_image_free(px);
        }
    }
    if (!img.valid()) {
        e.state = State::Failed;
        e.error = bytes.empty() ? "unreadable file" : std::format("cannot decode: {}", stbi_failure_reason());
        AE_LOG_WARN(kLog, "{}: {}", key, e.error);
        return;
    }
    const rhi::TextureHandle tex = create_texture(false, "Thumbnail.Image");
    if (!tex.is_valid()) {
        e.state = State::Failed;
        e.error = "texture creation failed";
        return;
    }
    device_.update_texture(tex, ByteSpan(reinterpret_cast<const byte*>(img.rgba8.data()), img.rgba8.size()), false);
    publish(e, tex);
    ++counters_.decoded;
}

// =================================================================================================
// models / prefabs / scenes (GPU)
// =================================================================================================
bool ThumbnailCache::ensure_renderer() {
    if (renderer_) {
        return true;
    }
    if (renderer_failed_) {
        return false;
    }
    auto r = renderer::Renderer::create(renderer::RendererDesc{ &device_, UVec2(kSize) });
    if (!r) {
        AE_LOG_ERROR(kLog, "thumbnail renderer unavailable: {}", r.error().message);
        renderer_failed_ = true;
        return false;
    }
    renderer_ = std::move(*r);
    renderer::RendererSettings& s = renderer_->settings();
    s.taa              = false; // one frame must be final: no history-based effects
    s.bloom            = false;
    s.gpu_culling      = false; // no occlusion history shared between unrelated thumbnails
    s.draw_debug_lines = false;
    s.shadows          = shadows_;
    cache_ = std::make_unique<gameplay::RenderResourceCache>(*renderer_, &assets_);

    constexpr u32          w = 128, h = 64;
    const std::vector<f32> sky = gameplay::make_sky_equirect(w, h, gameplay::SkySettings{});
    renderer::EnvironmentUpload up;
    up.width      = w;
    up.height     = h;
    up.rgba32f    = sky;
    up.debug_name = "ThumbnailSky";
    sky_ = renderer_->register_environment(up);
    AE_LOG_INFO(kLog, "thumbnail renderer ready ({}x{})", kSize, kSize);
    return true;
}

void ThumbnailCache::start_job(const std::string& key) {
    Entry& e = entries_.at(key);
    if (!ensure_renderer()) {
        e.state = State::Failed;
        e.error = "no thumbnail renderer";
        return;
    }
    auto world = std::make_unique<World>();
    gameplay::register_default_codecs(*world);
    register_editor_codecs(*world);
    const std::filesystem::path abs = root_ / key;
    Result<void>                r;
    switch (e.kind) {
    case ThumbnailKind::Model: {
        gameplay::SceneInstantiateOptions o;
        o.add_animators = false; // bind pose
        auto inst = gameplay::instantiate_model(assets_, *world, key, o);
        if (!inst) r = inst.error();
        break;
    }
    case ThumbnailKind::Prefab: {
        auto roots = instantiate_prefab(*world, abs, key, kNullEntity, Mat4(1.0f));
        if (!roots) r = roots.error();
        break;
    }
    case ThumbnailKind::Scene: {
        auto loaded = scene::load_scene(*world, abs);
        if (!loaded) r = loaded.error();
        break;
    }
    default: r = Error{ ErrorCode::Unsupported, "not a rendered thumbnail kind" }; break;
    }
    if (!r) {
        e.state = State::Failed;
        e.error = r.error().message;
        AE_LOG_WARN(kLog, "{}: {}", key, e.error);
        return;
    }
    world->update_transforms();
    job_       = Job{};
    job_.key   = key;
    job_.world = std::move(world);
}

void ThumbnailCache::advance_job() {
    ++job_.frames;
    cache_->update();
    World& w = *job_.world;

    // Bounds of every visible mesh; nothing is framed until all meshes are resident.
    AABB bounds{ Vec3(3.0e38f), Vec3(-3.0e38f) };
    u32  meshes = 0, missing = 0;
    for (const Entity ent : w.view<MeshRendererComponent>()) {
        if (const auto* vis = w.try_get<VisibilityComponent>(ent); vis != nullptr && !vis->visible) {
            continue;
        }
        ++meshes;
        const auto* mesh = cache_->mesh(w.get<MeshRendererComponent>(ent).mesh);
        if (mesh == nullptr) {
            ++missing;
            continue;
        }
        bounds.expand(gameplay::transform_aabb(w.world_matrix(ent), mesh->bounds));
    }
    if (meshes == 0) {
        finish_job(false, "nothing to render (no visible meshes)");
        return;
    }
    const bool timed_out = job_.frames >= kJobTimeoutFrames;
    if (missing == meshes && timed_out) {
        finish_job(false, "meshes never loaded");
        return;
    }
    if (missing > 0 && !timed_out) {
        return;
    }

    const ThumbnailCamera cam = frame_bounds(bounds, kFovY);
    gameplay::SceneExtractOptions opt;
    opt.viewport = UVec2(kSize);
    opt.camera   = gameplay::CameraOverride{ cam.view, kFovY, cam.near_z, cam.far_z };
    const gameplay::SceneExtractStats st = gameplay::extract_render_scene(w, *cache_, opt, job_.scene);
    // Materials / textures are requested by the extraction and finish over the next updates.
    if ((st.pending_meshes > 0 || cache_->stats().pending > 0) && !timed_out) {
        return;
    }
    if (job_.scene.lights.empty()) { // models / prefabs carry no lights: a key + a fill light
        renderer::RenderLight key;
        key.type      = renderer::LightType::Directional;
        key.direction = glm::normalize(Vec3(-0.45f, -0.8f, -0.4f));
        key.color     = Vec3(1.0f, 0.97f, 0.92f);
        key.intensity = 2.5f;
        key.cast_shadows = true;
        renderer::RenderLight fill = key;
        fill.direction    = glm::normalize(Vec3(0.6f, -0.3f, 0.7f));
        fill.intensity    = 0.6f;
        fill.cast_shadows = false;
        job_.scene.lights = { key, fill };
    }
    job_.scene.environment.skybox            = sky_;
    job_.scene.environment.ambient_intensity = 1.0f;
    job_.ready = true;
}

void ThumbnailCache::finish_job(bool ok, const std::string& error) {
    if (!ok) {
        if (auto it = entries_.find(job_.key); it != entries_.end()) {
            it->second.state = State::Failed;
            it->second.error = error;
        }
        AE_LOG_WARN(kLog, "{}: {}", job_.key, error);
    }
    job_ = Job{};
}

// =================================================================================================
// textures
// =================================================================================================
rhi::TextureHandle ThumbnailCache::create_texture(bool render_target, const char* name) {
    rhi::TextureDesc td;
    td.type   = rhi::TextureType::Tex2D;
    td.format = rhi::Format::RGBA8Unorm; // display-encoded bytes, shown as-is by ImGui
    td.width  = kSize;
    td.height = kSize;
    td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc |
               (render_target ? rhi::TextureUsage::ColorAttach : rhi::TextureUsage::TransferDst);
    td.debug_name = name;
    return device_.create_texture(td);
}

void ThumbnailCache::publish(Entry& e, rhi::TextureHandle texture) {
    retire(e);
    e.texture  = texture;
    e.imgui_id = rhi::imgui_add_texture(device_, texture, sampler_);
    e.state    = State::Ready;
    e.error.clear();
}

void ThumbnailCache::retire(Entry& e) {
    if (e.texture.is_valid() || e.imgui_id != 0) {
        retired_.push_back(Retired{ e.texture, e.imgui_id, frame_ });
    }
    e.texture  = {};
    e.imgui_id = 0;
}

void ThumbnailCache::check_file_changes() {
    for (auto& [key, e] : entries_) {
        if (e.kind == ThumbnailKind::None || e.state == State::Pending) {
            continue;
        }
        const auto t = mtime_of(root_ / key);
        if (t == e.mtime) {
            continue;
        }
        e.mtime = t;
        retire(e);
        e.state = State::Pending;
        (thumbnail_is_rendered(e.kind) ? render_queue_ : image_queue_).push_back(key);
        AE_LOG_INFO(kLog, "{} changed on disk: regenerating its thumbnail", key);
    }
}

} // namespace aether::editor
