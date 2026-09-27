// pipelines.h — the renderer's pipeline table + runtime shader/pipeline library.
//
// Private header. build_pipeline_specs() is the single source of truth for every shader
// permutation the renderer uses; the unit tests compile exactly this list. The
// PipelineLibrary compiles GLSL at runtime (Device::compile_glsl_file), creates
// pipelines, hot-reloads them (keeping the old pipeline whenever a shader fails) and
// creates per-output-format variants on demand (tonemap / debug lines write straight into
// the caller's RenderTarget, whose format is only known at render()). A pipeline whose
// shaders fail to compile stays invalid and its pass is skipped - never a crash.
// Main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"
#include "aether/rhi/device.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace aether::renderer {

inline constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16F;
inline constexpr rhi::Format kDepthFormat = rhi::Format::D32F;
inline constexpr rhi::Format kShadowFormat = rhi::Format::D32F;

struct ShaderSource {
    std::string                    file; // relative to the shader root (e.g. "renderer/mesh.vert")
    rhi::ShaderStage               stage = rhi::ShaderStage::Vertex;
    std::vector<rhi::ShaderDefine> defines;

    [[nodiscard]] bool        empty() const noexcept { return file.empty(); }
    [[nodiscard]] std::string key() const;
};

struct PipelineSpec {
    std::string               name;
    bool                      compute = false;
    // ADR-0010: mesh-shading pipeline. `vs` then holds the MESH shader and `ts` the task shader.
    bool                      mesh_shading = false;
    ShaderSource              ts;
    ShaderSource              vs;
    ShaderSource              fs;
    ShaderSource              cs;
    rhi::GraphicsPipelineDesc graphics{}; // shader handles filled by the library
    u32                       push_constant_size = 0;
    bool                      per_target_format = false; // color format patched per target
};

// ---- mesh pipelines (indexed by pass + permutation) ----
// Pick (ADR-0009): R32Uint id buffer over the finished depth prepass (Equal test, no writes).
enum class MeshPass : u8 { Depth, Shadow, Forward, Translucent, Overdraw, Pick };
inline constexpr u32 kMeshPipelineCount = 30;
inline constexpr rhi::Format kPickIdFormat = rhi::Format::R32Uint;
[[nodiscard]] u32 mesh_pipeline_index(MeshPass pass, bool skinned, bool masked, bool double_sided);

// ---- fixed pipelines (follow the mesh pipelines in the table) ----
enum class PipelineId : u32 {
    Sky = kMeshPipelineCount,
    LightCull,
    Ssao,
    SsaoBlur,
    Taa,
    BloomDown,
    BloomUp,
    EquirectToCube,
    Irradiance,
    Prefilter,
    BrdfLut,
    GpuCull,     // ADR-0009: GPU-driven instance culling (frustum / two-phase occlusion)
    HiZBuild,    // ADR-0009: reverse-Z min depth pyramid
    PickResolve, // ADR-0009: id buffer texel -> counter buffer
    Tonemap,    // per target format
    DebugLines, // per target format
    Count
};
inline constexpr u32 kPipelineCount = static_cast<u32>(PipelineId::Count);
[[nodiscard]] constexpr u32 pipeline_index(PipelineId id) noexcept { return static_cast<u32>(id); }

// ---- meshlet pipelines (ADR-0010; follow the fixed pipelines) ----
// Task + mesh shader variants of the passes the GPU-driven path draws (static meshes only):
// Depth (masked x double-sided), Forward (double-sided), Pick (double-sided). The slots stay
// empty (invalid pipelines) on devices without mesh shaders.
inline constexpr u32 kMeshletPipelineCount = 8;
inline constexpr u32 kPipelineTableSize = kPipelineCount + kMeshletPipelineCount;
[[nodiscard]] u32 meshlet_pipeline_index(MeshPass pass, bool masked, bool double_sided);

// Defines injected into every shader (constants shared with gpu_data.h).
[[nodiscard]] std::vector<rhi::ShaderDefine> shared_shader_defines();

// The full pipeline table, indexed by mesh_pipeline_index()/PipelineId.
[[nodiscard]] std::vector<PipelineSpec> build_pipeline_specs(const rhi::DeviceFeatures& features);

// Resolves the shader root: paths::shader_dir() if it contains renderer/, else the source
// tree baked in at build time (AE_RENDERER_SHADER_SOURCE_DIR), else shader_dir().
[[nodiscard]] std::filesystem::path resolve_shader_root();

class PipelineLibrary {
public:
    PipelineLibrary(rhi::Device& device, std::filesystem::path shader_root);
    ~PipelineLibrary();
    PipelineLibrary(const PipelineLibrary&) = delete;
    PipelineLibrary& operator=(const PipelineLibrary&) = delete;

    // Compiles every shader and creates every pipeline. Returns the number of pipelines
    // that failed (they stay invalid; their passes are disabled).
    u32 build(std::vector<PipelineSpec> specs);

    // Recompiles everything from disk. Pipelines whose shaders all compile are replaced;
    // the others keep their previous version. Returns an error listing failures.
    Result<void> reload();

    [[nodiscard]] rhi::PipelineHandle get(u32 index) const;
    [[nodiscard]] rhi::PipelineHandle get(PipelineId id) const { return get(pipeline_index(id)); }
    // Variant of a per_target_format pipeline for `color_format` (created lazily).
    [[nodiscard]] rhi::PipelineHandle get_for_format(PipelineId id, rhi::Format color_format);
    [[nodiscard]] bool valid(PipelineId id) const { return get(id).is_valid(); }

    void shutdown();

private:
    struct Entry {
        PipelineSpec                                           spec;
        rhi::ShaderHandle                                      ts, vs, fs, cs;
        rhi::PipelineHandle                                    pipeline;
        std::vector<std::pair<rhi::Format, rhi::PipelineHandle>> variants;
        bool                                                   failed_logged = false;
    };
    struct CompiledShader {
        std::string       key;
        rhi::ShaderHandle handle;
        bool              ok = false;
    };

    // Compiles (or fetches from `cache`) one shader module.
    rhi::ShaderHandle compile(const ShaderSource& src, std::vector<CompiledShader>& cache,
                              std::string& errors);
    rhi::PipelineHandle create(const Entry& e, rhi::ShaderHandle ts, rhi::ShaderHandle vs, rhi::ShaderHandle fs,
                               rhi::ShaderHandle cs, rhi::Format color_override);
    void destroy_unreferenced_shaders();

    rhi::Device&                   device_;
    std::filesystem::path          root_;
    std::vector<Entry>             entries_;
    std::vector<rhi::ShaderHandle> modules_; // every live shader module
};

} // namespace aether::renderer
