// aether/rhi/resources.h — resource handles + creation descriptors.
// FROZEN CONTRACT (ADR-0001). Handles are opaque; the backend owns the objects.
#pragma once

#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/rhi/enums.h"

#include <string>
#include <vector>

namespace aether::rhi {

// Opaque, generational handles to backend resources.
struct BufferTag; struct TextureTag; struct SamplerTag; struct PipelineTag;
struct ShaderTag; struct DescriptorTag;
using BufferHandle     = Handle<BufferTag>;
using TextureHandle    = Handle<TextureTag>;
using SamplerHandle    = Handle<SamplerTag>;
using PipelineHandle   = Handle<PipelineTag>;
using ShaderHandle     = Handle<ShaderTag>;
using DescriptorHandle = Handle<DescriptorTag>; // index into the bindless global set

struct BufferDesc {
    u64         size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    std::string debug_name;
};

struct TextureDesc {
    TextureType  type = TextureType::Tex2D;
    Format       format = Format::RGBA8Unorm;
    u32          width = 1, height = 1, depth = 1;
    u32          mip_levels = 1;
    u32          array_layers = 1;
    u32          samples = 1;
    TextureUsage usage = TextureUsage::Sampled;
    std::string  debug_name;
};

struct SamplerDesc {
    Filter      min_filter = Filter::Linear;
    Filter      mag_filter = Filter::Linear;
    MipmapMode  mipmap = MipmapMode::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    AddressMode address_w = AddressMode::Repeat;
    f32         max_anisotropy = 1.0f;
    bool        compare_enable = false;
    CompareOp   compare_op = CompareOp::Always;
    f32         min_lod = 0.0f, max_lod = 1000.0f;
};

// A compiled shader module + its stage. SPIR-V produced offline or at runtime
// (glslang wrapper, see rhi::compile_glsl).
struct ShaderDesc {
    ShaderStage         stage = ShaderStage::Vertex;
    std::vector<u32>    spirv;         // SPIR-V words
    std::string         entry_point = "main";
    std::string         debug_name;
};

// Render target formats for dynamic-rendering pipelines (no VkRenderPass).
struct RenderTargetFormats {
    std::vector<Format> color;
    Format              depth = Format::Undefined;
    u32                 samples = 1;
};

struct BlendState {
    bool        enable = false;
    BlendFactor src_color = BlendFactor::SrcAlpha;
    BlendFactor dst_color = BlendFactor::OneMinusSrcAlpha;
    BlendOp     color_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One;
    BlendFactor dst_alpha = BlendFactor::Zero;
    BlendOp     alpha_op = BlendOp::Add;
};

struct DepthState {
    bool      test = true;
    bool      write = true;
    CompareOp compare = CompareOp::GreaterEqual; // reverse-Z default
    bool      bias_enable = false;  // values set dynamically via CommandList::set_depth_bias
    bool      clamp_enable = false; // depth clamp (shadow pancaking); gated on device support
};

// ADR-0009 (additive): `binding` selects the VertexBinding the attribute is sourced from.
// If no declared VertexBinding has that number, the attribute falls back to the first
// declared binding (the pre-M2 behaviour, so single-binding layouts need not set it).
struct VertexAttribute { u32 location; u32 offset; Format format; u32 binding = 0; };
struct VertexBinding    { u32 binding; u32 stride; bool per_instance = false; };

struct GraphicsPipelineDesc {
    ShaderHandle              vertex;
    ShaderHandle              fragment;
    std::vector<VertexBinding>   vertex_bindings;
    std::vector<VertexAttribute> vertex_attributes;
    PrimitiveTopology         topology = PrimitiveTopology::TriangleList;
    CullMode                  cull = CullMode::Back;
    FrontFace                 front_face = FrontFace::CounterClockwise;
    PolygonMode               polygon = PolygonMode::Fill;
    DepthState                depth{};
    BlendState                blend{};
    RenderTargetFormats       targets{};
    u32                       push_constant_size = 0;
    std::string               debug_name;
};

struct ComputePipelineDesc {
    ShaderHandle compute;
    u32          push_constant_size = 0;
    std::string  debug_name;
};

// A color attachment binding for dynamic rendering.
struct ColorAttachment {
    TextureHandle texture;
    u32           mip = 0;   // subresource rendered to (view created/cached by the backend)
    u32           layer = 0;
    LoadOp        load = LoadOp::Clear;
    StoreOp       store = StoreOp::Store;
    Vec4          clear_color{ 0, 0, 0, 1 };
};
struct DepthAttachment {
    TextureHandle texture;
    u32           mip = 0;
    u32           layer = 0; // e.g. shadow cascade index in a Tex2DArray
    LoadOp        load = LoadOp::Clear;
    StoreOp       store = StoreOp::Store;
    f32           clear_depth = 0.0f; // reverse-Z clear
    u32           clear_stencil = 0;
};

struct RenderingInfo {
    UVec2                        render_area{ 0, 0 };
    std::vector<ColorAttachment> color;
    DepthAttachment              depth{};
    bool                         has_depth = false;
};

// Runtime shader compilation options (glslang). #include is resolved relative to the
// including file, then each include_dir, then paths::shader_dir().
struct ShaderDefine {
    std::string name;
    std::string value;
};
struct ShaderCompileOptions {
    std::vector<ShaderDefine> defines;
    std::vector<std::string>  include_dirs;
    bool                      debug_info = false; // emit OpLine/names for RenderDoc
};

} // namespace aether::rhi
