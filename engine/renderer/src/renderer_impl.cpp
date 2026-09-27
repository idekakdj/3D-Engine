// renderer_impl.cpp — Renderer creation, resource registration/release, settings, reload.
#include "renderer_impl.h"

#include "format_utils.h"

#include "aether/core/log.h"
#include "aether/renderer/instance_flags.h"

#include <algorithm>
#include <iterator>
#include <limits>

namespace aether::renderer {

namespace {
constexpr const char* kLogCat = "Renderer";

u32 descriptor_index(rhi::DescriptorHandle h) { return h.is_valid() ? h.index() : kGpuInvalidIndex; }
} // namespace

// ===========================================================================
// Factory
// ===========================================================================
Result<std::unique_ptr<Renderer>> Renderer::create(const RendererDesc& desc) {
    if (!desc.device) {
        return make_error<std::unique_ptr<Renderer>>(ErrorCode::InvalidArgument,
                                                     "RendererDesc::device is null");
    }
    auto impl = std::make_unique<RendererImpl>(*desc.device, desc.output_size);
    if (auto r = impl->initialize(); !r) {
        return Result<std::unique_ptr<Renderer>>(r.error());
    }
    return Result<std::unique_ptr<Renderer>>(std::unique_ptr<Renderer>(std::move(impl)));
}

RendererImpl::RendererImpl(rhi::Device& device, UVec2 output_size)
    : device_(device), output_size_(glm::max(output_size, UVec2(1))) {}

Result<void> RendererImpl::initialize() {
    const rhi::DeviceFeatures& f = device_.features();
    frames_in_flight_ = std::max(1u, device_.frames_in_flight());
    if (!f.buffer_device_address || !f.descriptor_indexing || !f.dynamic_rendering) {
        AE_LOG_WARN(kLogCat, "device reports missing BDA/bindless/dynamic rendering - rendering may fail");
    }

    create_samplers();
    if (!linear_clamp_.is_valid() || !point_clamp_.is_valid()) {
        return make_error(ErrorCode::Internal, "renderer sampler creation failed");
    }

    pipelines_ = std::make_unique<PipelineLibrary>(device_, resolve_shader_root());
    const u32 failures = pipelines_->build(build_pipeline_specs(f));
    if (failures > 0) {
        AE_LOG_ERROR(kLogCat, "{} pipelines failed to build; affected passes are disabled", failures);
    }

    if (!geometry_.init(device_, GeometryArena::Capacities{})) {
        return make_error(ErrorCode::OutOfMemory, "geometry arena creation failed");
    }
    frame_arena_.init(device_, frames_in_flight_, 4ull << 20);

    rg_pool_ = std::make_unique<RGResourcePool>(
        device_, RGResourcePool::Samplers{ linear_clamp_, point_clamp_ }, 4);
    graph_ = std::make_unique<RenderGraph>(device_, *rg_pool_);

    material_table_.assign(1, GpuMaterial{});
    // Default material (invalid/released handles): neutral grey dielectric.
    material_table_[0].base_color = Vec4(0.8f, 0.8f, 0.8f, 1.0f);
    material_table_[0].metallic = 0.0f;
    material_table_[0].roughness = 0.5f;

    ensure_brdf_lut();
    AE_LOG_INFO(kLogCat, "renderer ready ({}x{}, {} frames in flight)", output_size_.x, output_size_.y,
                frames_in_flight_);
    return {};
}

RendererImpl::~RendererImpl() {
    device_.wait_idle();
    process_releases(true);

    meshes_.drain([&](MeshRecord&& m) { geometry_.release(m.geo); });
    textures_.drain([&](TextureRecord&& t) {
        if (t.descriptor.is_valid()) {
            device_.unregister_texture(t.descriptor);
        }
        device_.destroy(t.texture);
    });
    environments_.drain([&](EnvRecord&& e) { destroy_environment_maps(device_, e.maps); });
    materials_.drain([](MaterialRecord&&) {});

    destroy_ibl_texture(device_, brdf_lut_);
    destroy_persistent(shadow_map_);
    destroy_persistent(history_[0]);
    destroy_persistent(history_[1]);
    for (rhi::BufferHandle* b : { &visibility_buffer_, &readback_buffer_ }) {
        if (b->is_valid()) {
            device_.destroy(*b);
        }
        *b = {};
    }

    graph_.reset();
    rg_pool_.reset();
    frame_arena_.shutdown();
    geometry_.shutdown();
    if (pipelines_) {
        pipelines_->shutdown();
        pipelines_.reset();
    }
    destroy_samplers();
}

void RendererImpl::create_samplers() {
    const rhi::DeviceFeatures& f = device_.features();
    rhi::SamplerDesc s;
    s.max_anisotropy = f.sampler_anisotropy ? 16.0f : 1.0f;
    material_sampler_ = device_.create_sampler(s);

    rhi::SamplerDesc c;
    c.address_u = c.address_v = c.address_w = rhi::AddressMode::ClampToEdge;
    linear_clamp_ = device_.create_sampler(c);

    rhi::SamplerDesc p = c;
    p.min_filter = p.mag_filter = rhi::Filter::Nearest;
    p.mipmap = rhi::MipmapMode::Nearest;
    point_clamp_ = device_.create_sampler(p);

    rhi::SamplerDesc sh = c;
    sh.mipmap = rhi::MipmapMode::Nearest;
    sh.compare_enable = true;
    sh.compare_op = rhi::CompareOp::GreaterEqual; // reverse-Z: lit when receiver >= occluder
    sh.max_lod = 0.0f;
    shadow_sampler_ = device_.create_sampler(sh);
}

void RendererImpl::destroy_samplers() {
    for (rhi::SamplerHandle* h : { &material_sampler_, &linear_clamp_, &point_clamp_, &shadow_sampler_ }) {
        if (h->is_valid()) {
            device_.destroy(*h);
        }
        *h = {};
    }
}

void RendererImpl::ensure_brdf_lut() {
    if (brdf_lut_.texture.is_valid() || !pipelines_->valid(PipelineId::BrdfLut)) {
        return;
    }
    auto lut = create_brdf_lut(device_, *pipelines_, IblSamplers{ linear_clamp_, material_sampler_ });
    if (lut) {
        brdf_lut_ = lut.value();
    } else {
        AE_LOG_ERROR(kLogCat, "BRDF LUT generation failed: {}", lut.error().message);
    }
}

void RendererImpl::destroy_persistent(PersistentTexture& t) {
    if (t.sampled.is_valid()) {
        device_.unregister_texture(t.sampled);
    }
    if (t.storage.is_valid()) {
        device_.unregister_storage_texture(t.storage);
    }
    if (t.texture.is_valid()) {
        device_.destroy(t.texture);
    }
    t = PersistentTexture{};
}

void RendererImpl::defer_release(std::function<void()> fn) {
    // The last GPU use happened no later than the frame recorded by the latest render();
    // frames_in_flight renders later Device::begin_frame() has waited for it.
    releases_.push_back(PendingRelease{ frame_counter_ + frames_in_flight_ + 1, std::move(fn) });
}

void RendererImpl::process_releases(bool force_all) {
    auto it = std::stable_partition(releases_.begin(), releases_.end(), [&](const PendingRelease& r) {
        return !force_all && r.frame > frame_counter_;
    });
    std::vector<PendingRelease> ready;
    ready.reserve(static_cast<usize>(std::distance(it, releases_.end())));
    std::move(it, releases_.end(), std::back_inserter(ready));
    releases_.erase(it, releases_.end());
    for (PendingRelease& r : ready) {
        r.fn();
    }
}

// ===========================================================================
// Meshes
// ===========================================================================
MeshHandle RendererImpl::register_mesh(const MeshUpload& up) {
    if (up.vertices.empty() || up.indices.empty() || (up.indices.size() % 3) != 0) {
        AE_LOG_ERROR(kLogCat, "register_mesh('{}'): empty data or index count not a multiple of 3",
                     up.debug_name);
        return {};
    }
    if (!up.skin.empty() && up.skin.size() != up.vertices.size()) {
        AE_LOG_ERROR(kLogCat, "register_mesh('{}'): skin stream size {} != vertex count {}",
                     up.debug_name, up.skin.size(), up.vertices.size());
        return {};
    }
    if (up.vertices.size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
        AE_LOG_ERROR(kLogCat, "register_mesh('{}'): too many vertices", up.debug_name);
        return {};
    }
    const u32 index_count = static_cast<u32>(up.indices.size());
    MeshRecord rec;
    rec.name = up.debug_name;
    if (up.submeshes.empty()) {
        rec.submeshes.push_back(Submesh{ 0, index_count, 0, up.bounds });
    } else {
        rec.submeshes.assign(up.submeshes.begin(), up.submeshes.end());
        for (const Submesh& s : rec.submeshes) {
            if (static_cast<u64>(s.first_index) + s.index_count > index_count) {
                AE_LOG_ERROR(kLogCat, "register_mesh('{}'): submesh range out of bounds", up.debug_name);
                return {};
            }
        }
    }
    rec.bounds = up.bounds;
    if (aabb_is_unset(rec.bounds) || !rec.bounds.valid()) {
        rec.bounds.min = rec.bounds.max = up.vertices[0].position;
        for (const Vertex& v : up.vertices) {
            rec.bounds.expand(v.position);
        }
    }
    for (Submesh& s : rec.submeshes) {
        if (aabb_is_unset(s.bounds) || !s.bounds.valid()) {
            s.bounds = rec.bounds;
        }
    }

    auto geo = geometry_.upload(up.vertices, up.skin, up.indices);
    if (!geo) {
        AE_LOG_ERROR(kLogCat, "register_mesh('{}'): geometry upload failed", up.debug_name);
        return {};
    }
    rec.geo = *geo;
    const MeshHandle h = meshes_.insert(std::move(rec));
    if (!h.is_valid()) {
        geometry_.release(*geo);
    }
    return h;
}

void RendererImpl::release(MeshHandle h) {
    auto rec = meshes_.remove(h);
    if (!rec) {
        return;
    }
    const GeometryAllocation geo = rec->geo;
    defer_release([this, geo] { geometry_.release(geo); });
}

// ===========================================================================
// Textures
// ===========================================================================
TextureHandle RendererImpl::register_texture(const TextureUpload& up) {
    const bool bc = is_block_compressed(up.format);
    const bool format_ok = bc || up.format == rhi::Format::RGBA8Srgb || up.format == rhi::Format::RGBA8Unorm ||
                           up.format == rhi::Format::RGBA16F || up.format == rhi::Format::RGBA32F;
    const u32 layers = up.cubemap ? 6u : std::max(1u, up.array_layers);
    if (!format_ok || up.width == 0 || up.height == 0 || (up.cubemap && up.array_layers != 6)) {
        AE_LOG_ERROR(kLogCat, "register_texture('{}'): unsupported format or size", up.debug_name);
        return {};
    }
    if (bc && !device_.features().texture_compression_bc) {
        AE_LOG_ERROR(kLogCat, "register_texture('{}'): the device does not support BC texture compression",
                     up.debug_name);
        return {};
    }
    const u32 max_mips = full_mip_count(up.width, up.height);
    const u32 supplied = std::max(1u, up.mip_levels);
    if (supplied > max_mips) {
        AE_LOG_ERROR(kLogCat, "register_texture('{}'): {} mips supplied, a {}x{} chain has at most {}",
                     up.debug_name, supplied, up.width, up.height, max_mips);
        return {};
    }
    const u64 expected = texture_upload_size(up.format, up.width, up.height, layers, supplied);
    if (up.pixels.size() != expected) {
        AE_LOG_ERROR(kLogCat, "register_texture('{}'): {} bytes given, {} expected ({} mips x {} layers)",
                     up.debug_name, up.pixels.size(), expected, supplied, layers);
        return {};
    }
    // Pre-built chains (and every BCn texture) are uploaded as-is; only a single uncompressed
    // mip is expanded on the GPU when generate_mips is set.
    const bool generate = up.generate_mips && supplied == 1 && !bc;
    rhi::TextureDesc d;
    d.type = up.cubemap ? rhi::TextureType::Cube
                        : (layers > 1 ? rhi::TextureType::Tex2DArray : rhi::TextureType::Tex2D);
    d.format = up.format;
    d.width = up.width;
    d.height = up.height;
    d.array_layers = layers;
    d.mip_levels = generate ? max_mips : supplied;
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst | rhi::TextureUsage::TransferSrc;
    d.debug_name = up.debug_name;
    TextureRecord rec;
    rec.texture = device_.create_texture(d);
    if (!rec.texture.is_valid()) {
        AE_LOG_ERROR(kLogCat, "register_texture('{}'): creation failed", up.debug_name);
        return {};
    }
    if (supplied == 1) {
        device_.update_texture(rec.texture, up.pixels, generate); // -> ShaderRead
    } else {
        // Layer-major within each mip (TextureUpload::mip_levels). Each call leaves its
        // subresource in ShaderRead; the first one initialises the whole image.
        u64 offset = 0;
        for (u32 m = 0; m < supplied; ++m) {
            const u64 bytes = subresource_size(up.format, std::max(1u, up.width >> m), std::max(1u, up.height >> m));
            for (u32 l = 0; l < layers; ++l) {
                device_.update_texture_mip(rec.texture, m, l, up.pixels.subspan(static_cast<usize>(offset),
                                                                                static_cast<usize>(bytes)));
                offset += bytes;
            }
        }
    }
    rec.descriptor = device_.register_texture(rec.texture, up.cubemap ? linear_clamp_ : material_sampler_);
    const TextureHandle h = textures_.insert(rec);
    materials_dirty_ = true; // a material may have been waiting for this handle
    return h;
}

void RendererImpl::release(TextureHandle h) {
    auto rec = textures_.remove(h);
    if (!rec) {
        return;
    }
    materials_dirty_ = true; // materials referencing it fall back to their factors
    const TextureRecord r = *rec;
    defer_release([this, r] {
        if (r.descriptor.is_valid()) {
            device_.unregister_texture(r.descriptor);
        }
        device_.destroy(r.texture);
    });
}

// ===========================================================================
// Materials
// ===========================================================================
GpuMaterial RendererImpl::make_gpu_material(const MaterialDesc& d) const {
    auto tex = [&](TextureHandle h) -> u32 {
        const TextureRecord* t = textures_.get(h);
        return t ? descriptor_index(t->descriptor) : kGpuInvalidIndex;
    };
    GpuMaterial m;
    m.base_color = d.base_color;
    m.emissive = d.emissive;
    m.metallic = std::clamp(d.metallic, 0.0f, 1.0f);
    m.roughness = std::clamp(d.roughness, 0.0f, 1.0f);
    m.normal_scale = d.normal_scale;
    m.occlusion_strength = std::clamp(d.occlusion_strength, 0.0f, 1.0f);
    m.alpha_cutoff = d.alpha_cutoff;
    m.base_color_tex = tex(d.base_color_tex);
    m.metallic_roughness_tex = tex(d.metallic_roughness_tex);
    m.normal_tex = tex(d.normal_tex);
    m.occlusion_tex = tex(d.occlusion_tex);
    m.emissive_tex = tex(d.emissive_tex);
    m.flags = (d.blend == BlendMode::Masked ? kMaterialFlagMasked : 0u) |
              (d.blend == BlendMode::Translucent ? kMaterialFlagTranslucent : 0u) |
              (d.double_sided ? kMaterialFlagDoubleSided : 0u);
    return m;
}

void RendererImpl::rebuild_material_table() {
    material_table_.resize(static_cast<usize>(materials_.capacity()) + 1);
    for (usize i = 1; i < material_table_.size(); ++i) {
        material_table_[i] = material_table_[0];
    }
    materials_.for_each([&](MaterialHandle h, MaterialRecord& rec) {
        material_table_[static_cast<usize>(h.index()) + 1] = make_gpu_material(rec.desc);
    });
    materials_dirty_ = false;
}

MaterialHandle RendererImpl::register_material(const MaterialDesc& desc) {
    const MaterialHandle h = materials_.insert(MaterialRecord{ desc });
    materials_dirty_ = true;
    return h;
}

void RendererImpl::update_material(MaterialHandle h, const MaterialDesc& desc) {
    if (MaterialRecord* rec = materials_.get(h)) {
        rec->desc = desc;
        materials_dirty_ = true;
    }
}

void RendererImpl::release(MaterialHandle h) {
    // The table is re-uploaded per frame slot, so the slot can be recycled immediately.
    if (materials_.remove(h)) {
        materials_dirty_ = true;
    }
}

// ===========================================================================
// Environments
// ===========================================================================
EnvHandle RendererImpl::register_environment(const EnvironmentUpload& up) {
    ensure_brdf_lut();
    auto maps = create_environment_maps(device_, *pipelines_, IblSamplers{ linear_clamp_, material_sampler_ }, up);
    if (!maps) {
        AE_LOG_ERROR(kLogCat, "register_environment('{}') failed: {}", up.debug_name, maps.error().message);
        return {};
    }
    const EnvHandle h = environments_.insert(EnvRecord{ maps.value() });
    if (!h.is_valid()) {
        destroy_environment_maps(device_, maps.value());
    }
    return h;
}

void RendererImpl::release(EnvHandle h) {
    auto rec = environments_.remove(h);
    if (!rec) {
        return;
    }
    EnvironmentMaps maps = rec->maps;
    defer_release([this, maps]() mutable { destroy_environment_maps(device_, maps); });
}

// ===========================================================================
// Settings / resize / reload
// ===========================================================================
void RendererImpl::resize(UVec2 output_size) {
    const UVec2 s = glm::max(output_size, UVec2(1));
    if (s == output_size_) {
        return;
    }
    // Transients re-key themselves through the pool; only the TAA history depends on size
    // and is recreated lazily on the next render().
    output_size_ = s;
    history_valid_ = false;
}

Result<void> RendererImpl::reload_shaders() {
    auto r = pipelines_->reload();
    ensure_brdf_lut(); // generate it now if its shader only just started compiling
    return r;
}

bool RendererImpl::shadows_active() const {
    return settings_.shadows && shadow_light_ >= 0 &&
           pipelines_->get(mesh_pipeline_index(MeshPass::Shadow, false, false, false)).is_valid();
}

} // namespace aether::renderer
