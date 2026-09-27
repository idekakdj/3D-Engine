// pipelines.cpp — pipeline table + runtime shader/pipeline library (see pipelines.h).
#include "pipelines.h"

#include "gpu_data.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"

#include <algorithm>
#include <format>
#include <system_error>

namespace aether::renderer {

namespace {
constexpr const char* kLogCat = "Renderer";
constexpr u32         kPushBytes = 128; // universal layout (ADR-0002)

rhi::ShaderDefine def(std::string name, std::string value = "1") {
    return rhi::ShaderDefine{ std::move(name), std::move(value) };
}

void mesh_vertex_layout(rhi::GraphicsPipelineDesc& d) {
    // Binding 0: aether::Vertex (48 bytes). Skinned meshes fetch their SkinVertex stream
    // through a buffer device address indexed by gl_VertexIndex (see mesh.vert), which keeps
    // one vertex layout (and one vertex-buffer bind) for every mesh permutation.
    d.vertex_bindings = { rhi::VertexBinding{ 0, 48, false } };
    d.vertex_attributes = {
        rhi::VertexAttribute{ 0, 0, rhi::Format::RGB32F },   // position
        rhi::VertexAttribute{ 1, 12, rhi::Format::RGB32F },  // normal
        rhi::VertexAttribute{ 2, 24, rhi::Format::RGBA32F }, // tangent (w = bitangent sign)
        rhi::VertexAttribute{ 3, 40, rhi::Format::RG32F },   // uv0
    };
}

PipelineSpec mesh_spec(MeshPass pass, bool skinned, bool masked, bool double_sided,
                       const rhi::DeviceFeatures& features) {
    PipelineSpec s;
    const char* pass_name = "";
    s.vs = { "renderer/mesh.vert", rhi::ShaderStage::Vertex, {} };
    if (skinned) {
        s.vs.defines.push_back(def("AE_SKINNED"));
    }
    rhi::GraphicsPipelineDesc& g = s.graphics;
    mesh_vertex_layout(g);
    g.topology = rhi::PrimitiveTopology::TriangleList;
    g.front_face = rhi::FrontFace::CounterClockwise; // Y-flipped projections keep CCW front
    g.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;

    switch (pass) {
    case MeshPass::Depth:
    case MeshPass::Shadow:
        pass_name = pass == MeshPass::Depth ? "Depth" : "Shadow";
        s.fs = { "renderer/depth.frag", rhi::ShaderStage::Fragment, {} };
        if (masked) {
            s.fs.defines.push_back(def("AE_ALPHA_MASK"));
        }
        g.depth = rhi::DepthState{ true, true, rhi::CompareOp::GreaterEqual, false, false };
        g.targets.depth = pass == MeshPass::Depth ? kDepthFormat : kShadowFormat;
        if (pass == MeshPass::Shadow) {
            g.depth.bias_enable = true;
            g.depth.clamp_enable = features.depth_clamp;
        }
        break;
    case MeshPass::Forward:
        pass_name = "Forward";
        s.fs = { "renderer/forward.frag", rhi::ShaderStage::Fragment, {} };
        // Opaque + masked after the prepass: Equal test, no writes, early-Z everywhere.
        g.depth = rhi::DepthState{ true, false, rhi::CompareOp::Equal, false, false };
        g.targets.color = { kHdrFormat };
        g.targets.depth = kDepthFormat;
        break;
    case MeshPass::Translucent:
        pass_name = "Translucent";
        s.fs = { "renderer/forward.frag", rhi::ShaderStage::Fragment, { def("AE_TRANSLUCENT") } };
        g.depth = rhi::DepthState{ true, false, rhi::CompareOp::GreaterEqual, false, false };
        g.blend = rhi::BlendState{ true,
                                   rhi::BlendFactor::SrcAlpha,
                                   rhi::BlendFactor::OneMinusSrcAlpha,
                                   rhi::BlendOp::Add,
                                   rhi::BlendFactor::One,
                                   rhi::BlendFactor::OneMinusSrcAlpha,
                                   rhi::BlendOp::Add };
        g.targets.color = { kHdrFormat };
        g.targets.depth = kDepthFormat;
        break;
    case MeshPass::Pick:
        pass_name = "Pick";
        s.fs = { "renderer/pick_id.frag", rhi::ShaderStage::Fragment, {} };
        g.depth = rhi::DepthState{ true, false, rhi::CompareOp::Equal, false, false };
        g.targets.color = { kPickIdFormat };
        g.targets.depth = kDepthFormat;
        break;
    case MeshPass::Overdraw:
        pass_name = "Overdraw";
        s.fs = { "renderer/overdraw.frag", rhi::ShaderStage::Fragment, {} };
        g.cull = rhi::CullMode::None;
        g.depth = rhi::DepthState{ false, false, rhi::CompareOp::Always, false, false };
        g.blend = rhi::BlendState{ true,
                                   rhi::BlendFactor::One,
                                   rhi::BlendFactor::One,
                                   rhi::BlendOp::Add,
                                   rhi::BlendFactor::One,
                                   rhi::BlendFactor::One,
                                   rhi::BlendOp::Add };
        g.targets.color = { kHdrFormat };
        g.targets.depth = kDepthFormat;
        break;
    }
    s.name = std::format("Mesh.{}{}{}{}", pass_name, skinned ? ".Skinned" : "",
                         masked ? ".Masked" : "", double_sided ? ".TwoSided" : "");
    g.debug_name = s.name;
    s.push_constant_size = kPushBytes;
    return s;
}

PipelineSpec compute_spec(std::string name, std::string file) {
    PipelineSpec s;
    s.name = std::move(name);
    s.compute = true;
    s.cs = { std::move(file), rhi::ShaderStage::Compute, {} };
    s.push_constant_size = kPushBytes;
    return s;
}
} // namespace

std::string ShaderSource::key() const {
    std::string k = file;
    k += '|';
    k += std::to_string(static_cast<u32>(stage));
    for (const rhi::ShaderDefine& d : defines) {
        k += '|';
        k += d.name;
        k += '=';
        k += d.value;
    }
    return k;
}

u32 mesh_pipeline_index(MeshPass pass, bool skinned, bool masked, bool double_sided) {
    const u32 sk = skinned ? 1u : 0u;
    const u32 m = masked ? 1u : 0u;
    const u32 ds = double_sided ? 1u : 0u;
    switch (pass) {
    case MeshPass::Depth: return 0u + sk * 4u + m * 2u + ds;
    case MeshPass::Shadow: return 8u + sk * 4u + m * 2u + ds;
    case MeshPass::Forward: return 16u + sk * 2u + ds;
    case MeshPass::Translucent: return 20u + sk * 2u + ds;
    case MeshPass::Overdraw: return 24u + sk;
    case MeshPass::Pick: return 26u + sk * 2u + ds;
    }
    return 0;
}

std::vector<rhi::ShaderDefine> shared_shader_defines() {
    return {
        def("AE_CLUSTER_X", std::to_string(kClusterX)),
        def("AE_CLUSTER_Y", std::to_string(kClusterY)),
        def("AE_CLUSTER_Z", std::to_string(kClusterZ)),
        def("AE_MAX_LIGHTS_PER_CLUSTER", std::to_string(kMaxLightsPerCluster)),
        def("AE_MAX_CASCADES", std::to_string(kMaxCascades)),
        def("AE_MAX_DRAW_BATCHES", std::to_string(kMaxDrawBatches)),
    };
}

std::vector<PipelineSpec> build_pipeline_specs(const rhi::DeviceFeatures& features) {
    std::vector<PipelineSpec> specs(kPipelineCount);

    for (const bool sk : { false, true }) {
        for (const bool m : { false, true }) {
            for (const bool ds : { false, true }) {
                for (MeshPass p : { MeshPass::Depth, MeshPass::Shadow }) {
                    specs[mesh_pipeline_index(p, sk, m, ds)] = mesh_spec(p, sk, m, ds, features);
                }
            }
            for (MeshPass p : { MeshPass::Forward, MeshPass::Translucent }) {
                specs[mesh_pipeline_index(p, sk, false, m)] = mesh_spec(p, sk, false, m, features);
            }
        }
        specs[mesh_pipeline_index(MeshPass::Overdraw, sk, false, false)] =
            mesh_spec(MeshPass::Overdraw, sk, false, false, features);
        for (const bool ds : { false, true }) {
            specs[mesh_pipeline_index(MeshPass::Pick, sk, false, ds)] = mesh_spec(MeshPass::Pick, sk, false, ds, features);
        }
    }

    {
        PipelineSpec s;
        s.name = "Sky";
        s.vs = { "renderer/sky.vert", rhi::ShaderStage::Vertex, {} };
        s.fs = { "renderer/sky.frag", rhi::ShaderStage::Fragment, {} };
        s.graphics.cull = rhi::CullMode::None;
        s.graphics.depth = rhi::DepthState{ true, false, rhi::CompareOp::GreaterEqual, false, false };
        s.graphics.targets.color = { kHdrFormat };
        s.graphics.targets.depth = kDepthFormat;
        s.graphics.debug_name = s.name;
        s.push_constant_size = kPushBytes;
        specs[pipeline_index(PipelineId::Sky)] = std::move(s);
    }
    specs[pipeline_index(PipelineId::LightCull)] = compute_spec("LightCull", "renderer/light_cull.comp");
    specs[pipeline_index(PipelineId::Ssao)] = compute_spec("SSAO", "renderer/ssao.comp");
    specs[pipeline_index(PipelineId::SsaoBlur)] = compute_spec("SSAO.Blur", "renderer/ssao_blur.comp");
    specs[pipeline_index(PipelineId::Taa)] = compute_spec("TAA", "renderer/taa.comp");
    specs[pipeline_index(PipelineId::BloomDown)] = compute_spec("Bloom.Down", "renderer/bloom_down.comp");
    specs[pipeline_index(PipelineId::BloomUp)] = compute_spec("Bloom.Up", "renderer/bloom_up.comp");
    specs[pipeline_index(PipelineId::EquirectToCube)] =
        compute_spec("IBL.EquirectToCube", "renderer/equirect_to_cube.comp");
    specs[pipeline_index(PipelineId::Irradiance)] = compute_spec("IBL.Irradiance", "renderer/irradiance.comp");
    specs[pipeline_index(PipelineId::Prefilter)] = compute_spec("IBL.Prefilter", "renderer/prefilter.comp");
    specs[pipeline_index(PipelineId::BrdfLut)] = compute_spec("IBL.BrdfLut", "renderer/brdf_lut.comp");
    specs[pipeline_index(PipelineId::GpuCull)] = compute_spec("GpuCull", "renderer/gpu_cull.comp");
    specs[pipeline_index(PipelineId::HiZBuild)] = compute_spec("HiZ.Build", "renderer/hiz_build.comp");
    specs[pipeline_index(PipelineId::PickResolve)] = compute_spec("Pick.Resolve", "renderer/pick_resolve.comp");
    {
        PipelineSpec s;
        s.name = "Tonemap";
        s.vs = { "renderer/fullscreen.vert", rhi::ShaderStage::Vertex, {} };
        s.fs = { "renderer/tonemap.frag", rhi::ShaderStage::Fragment, {} };
        s.graphics.cull = rhi::CullMode::None;
        s.graphics.depth = rhi::DepthState{ false, false, rhi::CompareOp::Always, false, false };
        s.graphics.targets.color = { rhi::Format::BGRA8Unorm };
        s.graphics.debug_name = s.name;
        s.push_constant_size = kPushBytes;
        s.per_target_format = true;
        specs[pipeline_index(PipelineId::Tonemap)] = std::move(s);
    }
    {
        PipelineSpec s;
        s.name = "DebugLines";
        s.vs = { "renderer/debug_line.vert", rhi::ShaderStage::Vertex, {} };
        s.fs = { "renderer/debug_line.frag", rhi::ShaderStage::Fragment, {} };
        s.graphics.topology = rhi::PrimitiveTopology::LineList;
        s.graphics.cull = rhi::CullMode::None;
        s.graphics.depth = rhi::DepthState{ false, false, rhi::CompareOp::Always, false, false };
        s.graphics.blend = rhi::BlendState{ true,
                                            rhi::BlendFactor::SrcAlpha,
                                            rhi::BlendFactor::OneMinusSrcAlpha,
                                            rhi::BlendOp::Add,
                                            rhi::BlendFactor::One,
                                            rhi::BlendFactor::OneMinusSrcAlpha,
                                            rhi::BlendOp::Add };
        s.graphics.targets.color = { rhi::Format::BGRA8Unorm };
        s.graphics.debug_name = s.name;
        s.push_constant_size = kPushBytes;
        s.per_target_format = true;
        specs[pipeline_index(PipelineId::DebugLines)] = std::move(s);
    }
    return specs;
}

std::filesystem::path resolve_shader_root() {
    std::error_code ec;
    std::filesystem::path root = paths::shader_dir();
    if (std::filesystem::exists(root / "renderer", ec)) {
        return root;
    }
#ifdef AE_RENDERER_SHADER_SOURCE_DIR
    const std::filesystem::path src(AE_RENDERER_SHADER_SOURCE_DIR);
    if (std::filesystem::exists(src / "renderer", ec)) {
        AE_LOG_INFO(kLogCat, "shader root '{}' has no renderer/; using source tree '{}'",
                    root.string(), src.string());
        return src;
    }
#endif
    return root;
}

// ===========================================================================
// PipelineLibrary
// ===========================================================================
PipelineLibrary::PipelineLibrary(rhi::Device& device, std::filesystem::path shader_root)
    : device_(device), root_(std::move(shader_root)) {}

PipelineLibrary::~PipelineLibrary() { shutdown(); }

rhi::ShaderHandle PipelineLibrary::compile(const ShaderSource& src, std::vector<CompiledShader>& cache,
                                           std::string& errors) {
    if (src.empty()) {
        return {};
    }
    const std::string key = src.key();
    for (const CompiledShader& c : cache) {
        if (c.key == key) {
            return c.ok ? c.handle : rhi::ShaderHandle{};
        }
    }
    CompiledShader entry;
    entry.key = key;

    rhi::ShaderCompileOptions opts;
    opts.defines = shared_shader_defines();
    opts.defines.insert(opts.defines.end(), src.defines.begin(), src.defines.end());
    opts.include_dirs = { root_.string() };
#ifndef NDEBUG
    opts.debug_info = true;
#endif
    auto spirv = device_.compile_glsl_file(src.stage, root_ / src.file, opts);
    if (!spirv) {
        errors += std::format("{}: {}\n", key, spirv.error().message);
        AE_LOG_ERROR(kLogCat, "shader compile failed [{}]:\n{}", key, spirv.error().message);
    } else {
        rhi::ShaderDesc sd;
        sd.stage = src.stage;
        sd.spirv = std::move(spirv.value());
        sd.debug_name = src.file;
        entry.handle = device_.create_shader(sd);
        entry.ok = entry.handle.is_valid();
        if (entry.ok) {
            modules_.push_back(entry.handle);
        } else {
            errors += std::format("{}: shader module creation failed\n", key);
        }
    }
    cache.push_back(entry);
    return entry.ok ? entry.handle : rhi::ShaderHandle{};
}

rhi::PipelineHandle PipelineLibrary::create(const Entry& e, rhi::ShaderHandle vs, rhi::ShaderHandle fs,
                                            rhi::ShaderHandle cs, rhi::Format color_override) {
    if (e.spec.compute) {
        if (!cs.is_valid()) {
            return {};
        }
        rhi::ComputePipelineDesc d;
        d.compute = cs;
        d.push_constant_size = e.spec.push_constant_size;
        d.debug_name = e.spec.name;
        return device_.create_compute_pipeline(d);
    }
    if (!vs.is_valid() || !fs.is_valid()) {
        return {};
    }
    rhi::GraphicsPipelineDesc d = e.spec.graphics;
    d.vertex = vs;
    d.fragment = fs;
    d.push_constant_size = e.spec.push_constant_size;
    if (color_override != rhi::Format::Undefined) {
        d.targets.color = { color_override };
    }
    return device_.create_graphics_pipeline(d);
}

u32 PipelineLibrary::build(std::vector<PipelineSpec> specs) {
    shutdown();
    entries_.clear();
    entries_.reserve(specs.size());
    std::vector<CompiledShader> cache;
    std::string                 errors;
    u32                         failures = 0;
    for (PipelineSpec& spec : specs) {
        Entry e;
        e.spec = std::move(spec);
        if (e.spec.name.empty()) {
            entries_.push_back(std::move(e)); // unused slot
            continue;
        }
        e.vs = compile(e.spec.vs, cache, errors);
        e.fs = compile(e.spec.fs, cache, errors);
        e.cs = compile(e.spec.cs, cache, errors);
        e.pipeline = create(e, e.vs, e.fs, e.cs, rhi::Format::Undefined);
        if (!e.pipeline.is_valid()) {
            ++failures;
            AE_LOG_ERROR(kLogCat, "pipeline '{}' unavailable - its pass is disabled", e.spec.name);
        }
        entries_.push_back(std::move(e));
    }
    destroy_unreferenced_shaders();
    return failures;
}

Result<void> PipelineLibrary::reload() {
    std::vector<CompiledShader> cache;
    std::string                 errors;
    u32                         replaced = 0;
    for (Entry& e : entries_) {
        if (e.spec.name.empty()) {
            continue;
        }
        const rhi::ShaderHandle vs = compile(e.spec.vs, cache, errors);
        const rhi::ShaderHandle fs = compile(e.spec.fs, cache, errors);
        const rhi::ShaderHandle cs = compile(e.spec.cs, cache, errors);
        const rhi::PipelineHandle np = create(e, vs, fs, cs, rhi::Format::Undefined);
        if (!np.is_valid()) {
            AE_LOG_WARN(kLogCat, "reload: keeping previous '{}'", e.spec.name);
            continue;
        }
        std::vector<std::pair<rhi::Format, rhi::PipelineHandle>> variants;
        bool                                                     ok = true;
        for (const auto& [fmt, old] : e.variants) {
            const rhi::PipelineHandle v = create(e, vs, fs, cs, fmt);
            if (!v.is_valid()) {
                ok = false;
                break;
            }
            variants.emplace_back(fmt, v);
        }
        if (!ok) {
            device_.destroy(np);
            for (auto& [fmt, v] : variants) {
                (void)fmt;
                device_.destroy(v);
            }
            AE_LOG_WARN(kLogCat, "reload: keeping previous '{}' (variant failed)", e.spec.name);
            continue;
        }
        if (e.pipeline.is_valid()) {
            device_.destroy(e.pipeline); // deferred by frames in flight inside the device
        }
        for (auto& [fmt, old] : e.variants) {
            (void)fmt;
            device_.destroy(old);
        }
        e.pipeline = np;
        e.variants = std::move(variants);
        e.vs = vs;
        e.fs = fs;
        e.cs = cs;
        ++replaced;
    }
    destroy_unreferenced_shaders();
    AE_LOG_INFO(kLogCat, "shader reload: {} pipelines rebuilt", replaced);
    if (!errors.empty()) {
        return make_error(ErrorCode::CompilationFailed, errors);
    }
    return {};
}

rhi::PipelineHandle PipelineLibrary::get(u32 index) const {
    return index < entries_.size() ? entries_[index].pipeline : rhi::PipelineHandle{};
}

rhi::PipelineHandle PipelineLibrary::get_for_format(PipelineId id, rhi::Format color_format) {
    const u32 index = pipeline_index(id);
    if (index >= entries_.size()) {
        return {};
    }
    Entry& e = entries_[index];
    if (!e.pipeline.is_valid()) {
        return {};
    }
    if (!e.spec.graphics.targets.color.empty() && e.spec.graphics.targets.color[0] == color_format) {
        return e.pipeline; // the base pipeline was built for this format
    }
    for (const auto& [fmt, p] : e.variants) {
        if (fmt == color_format) {
            return p;
        }
    }
    const rhi::PipelineHandle p = create(e, e.vs, e.fs, e.cs, color_format);
    if (p.is_valid()) {
        e.variants.emplace_back(color_format, p);
    }
    return p;
}

void PipelineLibrary::destroy_unreferenced_shaders() {
    std::vector<rhi::ShaderHandle> keep;
    for (const Entry& e : entries_) {
        for (rhi::ShaderHandle h : { e.vs, e.fs, e.cs }) {
            if (h.is_valid() && std::find(keep.begin(), keep.end(), h) == keep.end()) {
                keep.push_back(h);
            }
        }
    }
    for (rhi::ShaderHandle h : modules_) {
        if (std::find(keep.begin(), keep.end(), h) == keep.end()) {
            device_.destroy(h);
        }
    }
    modules_ = std::move(keep);
}

void PipelineLibrary::shutdown() {
    for (Entry& e : entries_) {
        if (e.pipeline.is_valid()) {
            device_.destroy(e.pipeline);
        }
        for (auto& [fmt, p] : e.variants) {
            (void)fmt;
            device_.destroy(p);
        }
        e.pipeline = {};
        e.variants.clear();
        e.vs = e.fs = e.cs = {};
    }
    for (rhi::ShaderHandle h : modules_) {
        device_.destroy(h);
    }
    modules_.clear();
}

} // namespace aether::renderer
