// gpu_data.h — C++ mirrors of every GPU-visible struct and push-constant block.
//
// Private header. All layouts use GLSL `scalar` block layout (GL_EXT_scalar_block_layout):
// members are aligned to their scalar component (4 bytes), buffer references to 8 bytes.
// glm types are tightly packed (vec3 = 12 bytes), so a C++ struct whose u64 members sit
// at 8-byte offsets matches the GLSL layout exactly. The static_asserts below pin every
// offset the shaders rely on; shaders/renderer/frame_data.glsl is the GLSL side.
//
// Compile-time constants shared with shaders are injected as #defines by
// shader_defines() (see pipelines.h), so the two sides cannot drift.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <cstddef>

namespace aether::renderer {

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
inline constexpr u32 kClusterX = 16;
inline constexpr u32 kClusterY = 9;
inline constexpr u32 kClusterZ = 24;
inline constexpr u32 kClusterCount = kClusterX * kClusterY * kClusterZ;
inline constexpr u32 kMaxLightsPerCluster = 128;
inline constexpr u32 kMaxCascades = 4;
inline constexpr u32 kMaxLights = 4096;       // uploaded per frame (extra lights are dropped)
inline constexpr u32 kGpuInvalidIndex = 0xFFFF'FFFFu;

// GpuLight::type
inline constexpr u32 kGpuLightDirectional = 0;
inline constexpr u32 kGpuLightPoint = 1;
inline constexpr u32 kGpuLightSpot = 2;

// GpuMaterial::flags
inline constexpr u32 kMaterialFlagMasked = 1u << 0;
inline constexpr u32 kMaterialFlagTranslucent = 1u << 1;
inline constexpr u32 kMaterialFlagDoubleSided = 1u << 2;

// GpuFrame::flags
inline constexpr u32 kFrameFlagShadows = 1u << 0;
inline constexpr u32 kFrameFlagIbl = 1u << 1;
inline constexpr u32 kFrameFlagReverseZ = 1u << 2; // always set; documents the convention

// MeshPush::pass_flags
inline constexpr u32 kPassFlagShadow = 1u << 0;
inline constexpr u32 kPassFlagOverdraw = 1u << 1;

// ---------------------------------------------------------------------------
// Per-instance record (one per RenderMeshInstance, indexed by gl_InstanceIndex).
// ---------------------------------------------------------------------------
struct GpuInstance {
    Mat4 model{ 1.0f };              // 0
    Vec4 normal_col[3]{};            // 64  inverse-transpose(model) columns (xyz)
    u32  material = 0;               // 112 index into the material table
    u32  flags = 0;                  // 116 instance_flags bits (renderer range only)
    u32  joint_offset = 0;           // 120 palette base in the frame joint buffer
    u32  joint_count = 0;            // 124
};
static_assert(sizeof(GpuInstance) == 128);
static_assert(offsetof(GpuInstance, normal_col) == 64);
static_assert(offsetof(GpuInstance, material) == 112);
static_assert(offsetof(GpuInstance, joint_offset) == 120);

struct GpuMaterial {
    Vec4 base_color{ 1.0f };               // 0
    Vec3 emissive{ 0.0f };                 // 16
    f32  metallic = 1.0f;                  // 28
    f32  roughness = 1.0f;                 // 32
    f32  normal_scale = 1.0f;              // 36
    f32  occlusion_strength = 1.0f;        // 40
    f32  alpha_cutoff = 0.5f;              // 44
    u32  base_color_tex = kGpuInvalidIndex;         // 48
    u32  metallic_roughness_tex = kGpuInvalidIndex; // 52
    u32  normal_tex = kGpuInvalidIndex;             // 56
    u32  occlusion_tex = kGpuInvalidIndex;          // 60
    u32  emissive_tex = kGpuInvalidIndex;           // 64
    u32  flags = 0;                                 // 68
    u32  pad0 = 0;
    u32  pad1 = 0;
};
static_assert(sizeof(GpuMaterial) == 80);
static_assert(offsetof(GpuMaterial, base_color_tex) == 48);
static_assert(offsetof(GpuMaterial, flags) == 68);

struct GpuLight {
    Vec3 position{ 0.0f };  // 0
    f32  range = 0.0f;      // 12
    Vec3 direction{ 0.0f, -1.0f, 0.0f }; // 16 direction the light travels (normalized)
    u32  type = 0;          // 28
    Vec3 color{ 0.0f };     // 32 color * intensity
    f32  spot_scale = 0.0f; // 44 1 / (cos_inner - cos_outer)
    f32  spot_offset = 0.0f;// 48 -cos_outer * spot_scale
    u32  shadow = kGpuInvalidIndex; // 52 0 = uses the cascaded shadow map
    u32  pad0 = 0;
    u32  pad1 = 0;
};
static_assert(sizeof(GpuLight) == 64);
static_assert(offsetof(GpuLight, color) == 32);
static_assert(offsetof(GpuLight, shadow) == 52);

struct GpuLineVertex {
    Vec3 position{ 0.0f };
    u32  color = 0xFFFF'FFFFu; // RGBA8, R in bits 0..7
};
static_assert(sizeof(GpuLineVertex) == 16);

// ---------------------------------------------------------------------------
// Per-frame constants. One per frame slot; referenced by every pass.
// ---------------------------------------------------------------------------
struct GpuFrame {
    u64  instances = 0;       // 0   InstanceBuffer
    u64  materials = 0;       // 8   MaterialBuffer
    u64  lights = 0;          // 16  LightBuffer
    u64  joints = 0;          // 24  JointBuffer
    u64  lines = 0;           // 32  LineBuffer
    u64  skin = 0;            // 40  SkinBuffer (geometry arena skin stream, gl_VertexIndex)
    Mat4 view{ 1.0f };        // 48
    Mat4 proj{ 1.0f };        // 112 jittered, reverse-Z
    Mat4 view_proj{ 1.0f };   // 176 jittered
    Mat4 inv_view{ 1.0f };    // 240
    Mat4 inv_proj{ 1.0f };    // 304 jittered
    Mat4 inv_view_proj{ 1.0f };        // 368 jittered
    Mat4 unjittered_view_proj{ 1.0f }; // 432
    Mat4 prev_view_proj{ 1.0f };       // 496 previous frame, unjittered
    Mat4 cascade_view_proj[kMaxCascades]{}; // 560
    Vec4 cascade_splits{ 0.0f };       // 816 view-space far distance of cascade i
    Vec4 cascade_texel_world{ 0.0f };  // 832 world-space size of one shadow texel
    Vec3 camera_pos{ 0.0f };           // 848
    f32  near_z = 0.1f;                // 860
    Vec2 viewport{ 1.0f };             // 864 internal render resolution
    Vec2 inv_viewport{ 1.0f };         // 872
    Vec2 jitter_ndc{ 0.0f };           // 880
    f32  far_z = 1000.0f;              // 888
    f32  exposure = 1.0f;              // 892
    Vec3 ambient{ 0.0f };              // 896 flat ambient radiance (no environment)
    f32  ibl_intensity = 1.0f;         // 908
    u32  light_count = 0;              // 912
    u32  directional_count = 0;        // 916
    u32  shadow_map = kGpuInvalidIndex;// 920 sampler2DArrayShadow
    u32  cascade_count = 0;            // 924
    u32  irradiance_map = kGpuInvalidIndex;  // 928 samplerCube
    u32  prefiltered_map = kGpuInvalidIndex; // 932 samplerCube
    u32  brdf_lut = kGpuInvalidIndex;        // 936 sampler2D (RG16F)
    u32  prefiltered_mips = 1;               // 940
    u32  skybox_map = kGpuInvalidIndex;      // 944 samplerCube
    f32  skybox_lod = 0.0f;                  // 948
    u32  debug_view = 0;                     // 952 renderer::DebugView
    u32  flags = 0;                          // 956 kFrameFlag*
    u32  cluster_x = kClusterX;              // 960
    u32  cluster_y = kClusterY;              // 964
    u32  cluster_z = kClusterZ;              // 968
    f32  cluster_z_scale = 0.0f;             // 972 slice = log(z) * scale - bias
    f32  cluster_z_bias = 0.0f;              // 976
    u32  cluster_tile_w = 1;                 // 980 pixels
    u32  cluster_tile_h = 1;                 // 984
    u32  frame_index = 0;                    // 988
    f32  shadow_distance = 0.0f;             // 992
    f32  shadow_fade_start = 0.0f;           // 996
    f32  shadow_normal_bias = 0.0f;          // 1000 in shadow texels
    f32  cascade_blend = 0.1f;               // 1004 fraction of a cascade used to blend
    u64  user_ids = 0;                       // 1008 UserIdBuffer: RenderMeshInstance::user_id (pick frames)
};
static_assert(offsetof(GpuFrame, view) == 48);
static_assert(offsetof(GpuFrame, cascade_view_proj) == 560);
static_assert(offsetof(GpuFrame, cascade_splits) == 816);
static_assert(offsetof(GpuFrame, camera_pos) == 848);
static_assert(offsetof(GpuFrame, ambient) == 896);
static_assert(offsetof(GpuFrame, light_count) == 912);
static_assert(offsetof(GpuFrame, irradiance_map) == 928);
static_assert(offsetof(GpuFrame, skybox_map) == 944);
static_assert(offsetof(GpuFrame, cluster_x) == 960);
static_assert(offsetof(GpuFrame, shadow_distance) == 992);
static_assert(offsetof(GpuFrame, user_ids) == 1008);
static_assert(sizeof(GpuFrame) == 1016);

// ---------------------------------------------------------------------------
// GPU-driven culling + picking (ADR-0009). GLSL side: shaders/renderer/cull_data.glsl.
// ---------------------------------------------------------------------------
// Opaque/masked candidates are grouped into batches by pipeline permutation and vertex
// arena (materials are bindless, so they never split a batch):
//   batch = skinned * 8 + masked * 4 + double_sided * 2 + skin_arena
inline constexpr u32 kMaxDrawBatches = 16;
inline constexpr u32 kCullGroupSize = 64;
inline constexpr u32 kHiZGroupSize = 8;

// GpuCullInstance::flags
inline constexpr u32 kCullFlagNeverCull = 1u << 0;

// Counter buffer (u32 words): per-batch draw counts of both phases, then the statistics
// and the pick result, which are copied to the CPU readback ring as one block.
inline constexpr u32 kCounterPhase1 = 0;                      // [0, 16)
inline constexpr u32 kCounterPhase2 = kMaxDrawBatches;        // [16, 32)
inline constexpr u32 kCounterStats = 2 * kMaxDrawBatches;     // 32: first word copied back
inline constexpr u32 kCounterFrustumCulled = kCounterStats + 0;
inline constexpr u32 kCounterOcclusionCulled = kCounterStats + 1;
inline constexpr u32 kCounterVisible = kCounterStats + 2;
inline constexpr u32 kCounterTriangles = kCounterStats + 3;
inline constexpr u32 kCounterPick = kCounterStats + 4;
inline constexpr u32 kReadbackWords = 8;                      // words copied per frame slot
inline constexpr u32 kCounterWords = kCounterStats + kReadbackWords;

// Cull phases (CullPush::phase).
inline constexpr u32 kCullPhaseFrustum = 0; // single pass: frustum only
inline constexpr u32 kCullPhase1 = 1;       // frustum + visible last frame
inline constexpr u32 kCullPhase2 = 2;       // frustum + Hi-Z occlusion; not drawn in phase 1

[[nodiscard]] constexpr u32 draw_batch_index(bool skinned, bool masked, bool double_sided,
                                             bool skin_arena) noexcept {
    return (skinned ? 8u : 0u) | (masked ? 4u : 0u) | (double_sided ? 2u : 0u) | (skin_arena ? 1u : 0u);
}

struct GpuCullInstance {
    Vec3 aabb_min{ 0.0f };   // 0  world space
    u32  instance = 0;       // 12 index into the frame instance buffer (= firstInstance)
    Vec3 aabb_max{ 0.0f };   // 16
    u32  batch = 0;          // 28 draw_batch_index()
    u32  first_index = 0;    // 32
    u32  index_count = 0;    // 36
    i32  vertex_offset = 0;  // 40
    u32  flags = 0;          // 44 kCullFlag*
};
static_assert(sizeof(GpuCullInstance) == 48);
static_assert(offsetof(GpuCullInstance, batch) == 28);

// VkDrawIndexedIndirectCommand.
struct GpuDrawCommand {
    u32 index_count = 0;
    u32 instance_count = 0;
    u32 first_index = 0;
    i32 vertex_offset = 0;
    u32 first_instance = 0;
};
static_assert(sizeof(GpuDrawCommand) == 20);

struct GpuCullView {
    Vec4  planes[6]{};                    // 0   main-view frustum (CPU-extracted, same as CPU culling)
    Mat4  view_proj{ 1.0f };              // 96  jittered: matches the depth buffer the Hi-Z is built from
    Vec2  viewport{ 1.0f };               // 160 depth-buffer extent in pixels
    UVec2 hzb_size{ 1 };                  // 168 Hi-Z mip-0 extent (power of two per axis)
    u32   hzb_mips = 0;                   // 176
    u32   hzb_tex = kGpuInvalidIndex;     // 180 sampler2D (R32F, point clamp), texelFetch per mip
    u32   plane_count = 6;                // 184 0 disables frustum culling
    u32   pad0 = 0;                       // 188
    u32   batch_offset[kMaxDrawBatches]{}; // 192 first command of each batch in a phase's array
};
static_assert(offsetof(GpuCullView, view_proj) == 96);
static_assert(offsetof(GpuCullView, hzb_size) == 168);
static_assert(offsetof(GpuCullView, batch_offset) == 192);
static_assert(sizeof(GpuCullView) == 256);

// ---------------------------------------------------------------------------
// Push-constant blocks (<= 128 bytes; the universal layout exposes them to all stages).
// ---------------------------------------------------------------------------
struct MeshPush {             // prepass, shadows, forward, overdraw
    u64 frame = 0;            // 0  FrameData
    u64 light_grid = 0;       // 8  ClusterGrid (forward only)
    u64 light_indices = 0;    // 16 ClusterIndices (forward only)
    u32 view_index = 0;       // 24 0 = main camera, 1 + i = cascade i
    u32 pass_flags = 0;       // 28 kPassFlag*
    u32 ssao_tex = kGpuInvalidIndex; // 32
    u32 pad = 0;
};
static_assert(sizeof(MeshPush) == 40);

struct CullPush {
    u64 candidates = 0;       // 0  CullInstanceBuffer
    u64 view = 0;             // 8  CullViewBuffer
    u64 draws = 0;            // 16 this phase's GpuDrawCommand array
    u64 counts = 0;           // 24 this phase's per-batch draw counts
    u64 stats = 0;            // 32 counter buffer base (kCounter* words)
    u64 visibility = 0;       // 40 per-instance visibility of the previous frame (occlusion)
    u32 count = 0;            // 48 candidates
    u32 phase = 0;            // 52 kCullPhase*
    u32 pad[2]{};
};
static_assert(sizeof(CullPush) == 64);

struct HiZPush {
    u32   src = 0;            // 0  level 0: depth (sampled); else previous mip (r32f storage)
    u32   dst = 0;            // 4  r32f storage view of the mip being written
    UVec2 src_size{ 1 };      // 8
    UVec2 dst_size{ 1 };      // 16
    u32   from_depth = 0;     // 24
    u32   pad = 0;
};
static_assert(sizeof(HiZPush) == 32);

struct PickPush {
    u64   counters = 0;       // 0  counter buffer (writes kCounterPick)
    u32   id_img = 0;         // 8  r32ui storage view of the id buffer
    u32   pad = 0;
    UVec2 pixel{ 0 };         // 16 internal-resolution pixel
};
static_assert(sizeof(PickPush) == 24);

struct LightCullPush {
    u64 frame = 0;
    u64 light_grid = 0;
    u64 light_indices = 0;
};
static_assert(sizeof(LightCullPush) == 24);

struct SkyPush {
    u64 frame = 0;
};

struct SsaoPush {
    u64  frame = 0;           // 0
    u32  depth_tex = 0;       // 8  full-res depth (sampled)
    u32  out_img = 0;         // 12 half-res RG16F storage (ao, linear depth)
    UVec2 out_size{ 0 };      // 16
    f32  radius = 0.5f;       // 24 world units
    f32  intensity = 1.5f;    // 28 exponent
    f32  bias = 0.02f;        // 32
    u32  sample_count = 12;   // 36
};
static_assert(sizeof(SsaoPush) == 40);

struct SsaoBlurPush {
    u32  src_tex = 0;         // RG16F (ao, linear depth)
    u32  dst_img = 0;
    UVec2 size{ 0 };
    IVec2 direction{ 1, 0 };
    f32  sharpness = 8.0f;
    u32  pad = 0;
};
static_assert(sizeof(SsaoBlurPush) == 32);

struct TaaPush {
    u64  frame = 0;           // 0
    u32  color_tex = 0;       // 8
    u32  depth_tex = 0;       // 12
    u32  history_tex = 0;     // 16
    u32  out_img = 0;         // 20
    UVec2 size{ 0 };          // 24
    f32  feedback = 0.9f;     // 32 history weight
    u32  reset = 0;           // 36
};
static_assert(sizeof(TaaPush) == 40);

struct BloomPush {
    u32  src_tex = 0;         // level to filter (sampled, linear clamp)
    u32  add_tex = kGpuInvalidIndex; // upsample: same-level downsample result
    u32  dst_img = 0;         // RGBA16F storage
    u32  flags = 0;           // bit0 = first downsample (Karis average)
    Vec2 src_texel{ 0.0f };   // 1 / src extent
    UVec2 dst_size{ 0 };
    f32  radius = 1.0f;       // upsample tent radius in src texels
    u32  pad[3]{};
};
static_assert(sizeof(BloomPush) == 48);

struct TonemapPush {
    u32  color_tex = 0;       // 0  HDR input (TAA output or scene color)
    u32  bloom_tex = kGpuInvalidIndex; // 4
    f32  exposure = 1.0f;     // 8
    f32  bloom_intensity = 0.0f; // 12 lerp factor toward the bloom average
    u32  debug_view = 0;      // 16
    u32  apply_oetf = 0;      // 20
    Vec2 inv_target_size{ 0.0f }; // 24
    f32  bloom_norm = 1.0f;   // 32 1 / level count (the up-chain sums every level)
    u32  pad = 0;
};
static_assert(sizeof(TonemapPush) == 40);

struct LinePush {
    u64  frame = 0;           // 0
    u32  depth_tex = kGpuInvalidIndex; // 8 scene depth (reverse-Z) for the manual depth test
    u32  decode_srgb = 0;     // 12 target is *Srgb => convert sRGB-authored colors to linear
    Vec2 inv_target_size{ 0.0f }; // 16
    f32  depth_bias = 0.0f;   // 24
    u32  pad = 0;
};
static_assert(sizeof(LinePush) == 32);

struct IblPush {
    u32  src_tex = 0;         // equirect (sampler2D) or environment cube (samplerCube)
    u32  dst_img = 0;         // image2DArray (cube faces) or image2D (BRDF LUT)
    u32  dst_size = 0;        // face / LUT edge length in texels
    u32  sample_count = 0;
    f32  roughness = 0.0f;    // prefilter
    f32  src_lod = 0.0f;      // equirect->cube source lod
    u32  src_size = 0;        // environment cube mip-0 edge (filtered importance sampling)
    u32  src_mips = 1;
};
static_assert(sizeof(IblPush) == 32);

} // namespace aether::renderer
