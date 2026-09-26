// aether/renderer/renderer.h — the renderer's public API.
//
// FROZEN CONTRACT (ADR-0002). The renderer:
//   * owns every GPU upload and every renderer resource (meshes, textures, materials, IBL)
//   * consumes one RenderScene per frame and never reads the ECS or the asset system
//   * records into a caller-provided rhi::CommandList and writes the final, tonemapped,
//     display-encoded image into a caller-provided RenderTarget (swapchain or offscreen)
// Depends only on core + rhi. The gameplay bridge converts assets -> *Upload structs.
#pragma once

#include "aether/core/error.h"
#include "aether/core/geometry.h"
#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/renderer/render_scene.h"
#include "aether/rhi/device.h"

#include <memory>
#include <string>

namespace aether::renderer {

// ---------------------------------------------------------------------------
// Upload descriptors. Spans are read only during the call (renderer copies to GPU).
// ---------------------------------------------------------------------------
struct MeshUpload {
    Span<const Vertex>     vertices;
    Span<const SkinVertex> skin;      // empty, or same count as vertices
    Span<const u32>        indices;
    Span<const Submesh>    submeshes; // empty => one submesh covering all indices
    AABB                   bounds{};
    std::string            debug_name;
};

struct TextureUpload {
    rhi::Format format = rhi::Format::RGBA8Srgb; // RGBA8Srgb | RGBA8Unorm | RGBA16F | RGBA32F
    u32         width = 0;
    u32         height = 0;
    u32         array_layers = 1;  // 6 for cubemaps
    bool        cubemap = false;
    bool        generate_mips = true;
    ByteSpan    pixels;            // mip 0, all layers, tightly packed
    std::string debug_name;
};

enum class BlendMode : u8 { Opaque = 0, Masked, Translucent };

struct MaterialDesc {
    Vec4          base_color{ 1.0f };
    Vec3          emissive{ 0.0f };
    f32           metallic = 1.0f;
    f32           roughness = 1.0f;
    f32           normal_scale = 1.0f;
    f32           occlusion_strength = 1.0f;
    f32           alpha_cutoff = 0.5f;
    BlendMode     blend = BlendMode::Opaque;
    bool          double_sided = false;
    TextureHandle base_color_tex;         // invalid handle => factor only
    TextureHandle metallic_roughness_tex;
    TextureHandle normal_tex;
    TextureHandle occlusion_tex;
    TextureHandle emissive_tex;
    std::string   debug_name;
};

// Equirectangular HDR environment. The renderer derives the skybox cubemap, diffuse
// irradiance, specular prefiltered mips and the BRDF LUT on the GPU.
struct EnvironmentUpload {
    u32             width = 0;
    u32             height = 0;
    Span<const f32> rgba32f;      // width*height*4 floats
    std::string     debug_name;
};

// ---------------------------------------------------------------------------
// Output target for render().
// ---------------------------------------------------------------------------
struct RenderTarget {
    rhi::TextureHandle texture;   // swapchain backbuffer or an offscreen color texture
    rhi::Format        format = rhi::Format::BGRA8Unorm;
    UVec2              extent{ 0, 0 };
    // State the texture is in when render() starts, and the state render() must leave
    // it in. Swapchain: Undefined -> ColorAttachment (so ImGui can draw on top; the
    // device performs ColorAttachment -> Present in end_frame). Editor viewport
    // texture: Undefined -> ShaderRead (sampled by ImGui).
    rhi::ResourceState initial_state = rhi::ResourceState::Undefined;
    rhi::ResourceState final_state   = rhi::ResourceState::ColorAttachment;
};
// Encoding rule (ADR-0002): if `format` is a *Unorm format the final pass applies the
// sRGB OETF in-shader; if it is a *Srgb format the hardware encodes and the shader must not.

enum class DebugView : u8 {
    None = 0, Albedo, Normals, Roughness, Metallic, AmbientOcclusion, Emissive,
    LightComplexity, ShadowCascades, Overdraw,
};

struct RendererSettings {
    bool      shadows = true;
    u32       shadow_map_size = 2048;
    u32       shadow_cascades = 4;
    f32       shadow_distance = 150.0f;
    bool      ssao = true;
    bool      bloom = true;
    f32       bloom_intensity = 0.04f;
    bool      taa = true;
    bool      ibl = true;
    bool      draw_debug_lines = true;
    bool      frustum_culling = true;
    DebugView debug_view = DebugView::None;
};

struct RendererStats {
    u32 instances_submitted = 0;
    u32 instances_visible = 0;
    u32 draw_calls = 0;
    u32 lights = 0;
    u32 triangles = 0;
    f64 cpu_record_ms = 0.0;
};

struct RendererDesc {
    rhi::Device* device = nullptr;
    UVec2        output_size{ 1280, 720 }; // internal render resolution (resizable)
};

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> create(const RendererDesc& desc);
    virtual ~Renderer() = default;

    // ---- resources (main thread) ----
    virtual MeshHandle     register_mesh(const MeshUpload&) = 0;
    virtual TextureHandle  register_texture(const TextureUpload&) = 0;
    virtual MaterialHandle register_material(const MaterialDesc&) = 0;
    virtual EnvHandle      register_environment(const EnvironmentUpload&) = 0;
    virtual void           update_material(MaterialHandle, const MaterialDesc&) = 0;

    // Destruction is deferred internally until the GPU no longer uses the resource.
    virtual void release(MeshHandle) = 0;
    virtual void release(TextureHandle) = 0;
    virtual void release(MaterialHandle) = 0;
    virtual void release(EnvHandle) = 0;

    // ---- frame ----
    // Record the whole frame for `scene` into `cmd`, writing to `target`. Must be called
    // between Device::begin_frame() and Device::end_frame(), outside any rendering scope.
    virtual void render(const RenderScene& scene, rhi::CommandList& cmd,
                        const RenderTarget& target) = 0;

    // Internal render resolution (e.g. editor viewport panel size). Cheap if unchanged.
    virtual void resize(UVec2 output_size) = 0;

    // Recompile all shaders from disk (hot reload). Keeps old pipelines on failure.
    virtual Result<void> reload_shaders() = 0;

    virtual RendererSettings&    settings() = 0;
    virtual const RendererStats& stats() const = 0;
};

} // namespace aether::renderer
