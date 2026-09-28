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
inline constexpr u32 kMaxSpotShadows = 8; // ADR-0012: shadow-casting spot lights per frame
inline constexpr u32 kMaxPointShadows = 4; // ADR-0015: shadow-casting point lights per frame
inline constexpr u32 kPointShadowFaces = 6; // one perspective view per cube face (+X,-X,+Y,-Y,+Z,-Z)
// GpuSpotShadow entries per frame: spots first, then 6 consecutive faces per point light.
inline constexpr u32 kMaxLocalShadowViews = kMaxSpotShadows + kMaxPointShadows * kPointShadowFaces;
// ADR-0016: GI probe volume. Each updated probe is captured as 6 cube faces whose views follow the
// shadow views in the GpuSpotShadow table (entry kMaxLocalShadowViews + probe_slot * 6 + face).
inline constexpr u32 kMaxGiProbesPerFrame = 64;
inline constexpr u32 kMaxGiProbeAxis = 64;      // probes per axis
inline constexpr u32 kMaxGiProbes = 32768;      // probes per volume
inline constexpr u32 kGiProjectThreads = 64;    // gi_project.comp workgroup size
// ADR-0017: reflection capture probes. One probe is captured per frame (6 faces), its views follow
// the GI capture views in the table.
inline constexpr u32 kMaxReflectionProbes = 8;
inline constexpr u32 kReflViewBase = kMaxLocalShadowViews + kMaxGiProbesPerFrame * kPointShadowFaces;
inline constexpr u32 kMaxViewTableEntries = kReflViewBase + kPointShadowFaces;
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
    u32  shadow = kGpuInvalidIndex; // 52 directional: 0 = the cascaded shadow map; spot: GpuSpotShadow index; point: first of its 6 face entries (ADR-0015)
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
    u64  spot_shadows = 0;                   // 1016 SpotShadowBuffer (ADR-0012)
    u32  spot_shadow_map = kGpuInvalidIndex; // 1024 sampler2DArray (depth, point clamp)
    u32  spot_shadow_count = 0;              // 1028
    // ADR-0016: irradiance probe volume (gi_counts.x == 0 => off; probes on a regular grid).
    u64  gi_probes = 0;                      // 1032 GiProbeBuffer
    Vec3 gi_min{ 0.0f };                     // 1040 world position of probe (0,0,0)
    f32  gi_intensity = 1.0f;                // 1052
    Vec3 gi_inv_spacing{ 1.0f };             // 1056 1 / probe spacing per axis
    f32  gi_normal_bias = 0.0f;              // 1068 world units along the surface normal
    UVec3 gi_counts{ 0 };                    // 1072 probes per axis (>= 2 each when on)
    u32  gi_pad = 0;                         // 1084
    // ADR-0017: reflection probes, smallest box first.
    u64  reflection_probes = 0;              // 1088 ReflectionProbeBuffer
    u32  reflection_probe_count = 0;         // 1096
    u32  reflection_pad = 0;                 // 1100
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
static_assert(offsetof(GpuFrame, spot_shadows) == 1016);
static_assert(offsetof(GpuFrame, spot_shadow_count) == 1028);
static_assert(offsetof(GpuFrame, gi_probes) == 1032);
static_assert(offsetof(GpuFrame, gi_counts) == 1072);
static_assert(offsetof(GpuFrame, reflection_probes) == 1088);
static_assert(sizeof(GpuFrame) == 1104);

// ADR-0017: one reflection capture probe (a prefiltered cube captured at `position`, box-projected
// onto `box_min..box_max`, faded out over `blend_distance` inside the box faces).
struct GpuReflectionProbe {
    Vec3 box_min{ 0.0f };          // 0
    f32  intensity = 1.0f;         // 12
    Vec3 box_max{ 0.0f };          // 16
    f32  blend_distance = 1.0f;    // 28
    Vec3 position{ 0.0f };         // 32
    u32  cube = kGpuInvalidIndex;  // 44 samplerCube (prefiltered GGX mips)
    u32  mips = 1;                 // 48
    u32  pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(GpuReflectionProbe) == 64);

// ADR-0016: one irradiance probe - L1 spherical harmonics pre-convolved with the clamped cosine
// and divided by pi, per colour channel: irradiance/pi (n) = c.x + dot(c.yzw, n).
struct GpuGiProbe {
    Vec4 r{ 0.0f };    // 0
    Vec4 g{ 0.0f };    // 16
    Vec4 b{ 0.0f };    // 32
    Vec4 meta{ 0.0f }; // 48 x = back-face fraction seen (probe inside geometry), y = 1 once captured
};
static_assert(sizeof(GpuGiProbe) == 64);

// Local-light shadow view (ADR-0012 spot; ADR-0015 point = 6 consecutive cube-face entries).
// GpuLight::shadow of a spot light = its index in this array.
struct GpuSpotShadow {
    Mat4 view_proj{ 1.0f };  // 0  reverse-Z perspective from the light
    f32  texel_scale = 0.0f; // 64 world texel size per metre from the light (2 tan(fov/2) / size)
    u32  layer = 0;          // 68
    u32  map = kGpuInvalidIndex; // 72 ADR-0015: bindless depth array holding `layer` (spot or point map)
    f32  pad1 = 0.0f;
};
static_assert(sizeof(GpuSpotShadow) == 80);

// ---------------------------------------------------------------------------
// GPU-driven culling + picking (ADR-0009). GLSL side: shaders/renderer/cull_data.glsl.
// ---------------------------------------------------------------------------
// Opaque/masked candidates are grouped into batches by pipeline permutation and vertex
// arena (materials are bindless, so they never split a batch):
//   batch = meshlet * 16 + skinned * 8 + masked * 4 + double_sided * 2 + skin_arena
// Meshlet batches (ADR-0010, static meshes on mesh-shading devices) are drawn with
// vkCmdDrawMeshTasksIndirectCountEXT; their command slot holds a GpuMeshTaskCommand.
inline constexpr u32 kMaxDrawBatches = 32;
inline constexpr u32 kBatchMeshlet = 16;
inline constexpr u32 kCullGroupSize = 64;
inline constexpr u32 kHiZGroupSize = 8;

// GpuCullInstance::flags
inline constexpr u32 kCullFlagNeverCull = 1u << 0;

// Counter buffer (u32 words): per-batch draw counts of both phases, then the statistics
// and the pick result, which are copied to the CPU readback ring as one block.
inline constexpr u32 kCounterPhase1 = 0;                      // [0, 16)
inline constexpr u32 kCounterPhase2 = kMaxDrawBatches;        // [32, 64)
inline constexpr u32 kCounterStats = 2 * kMaxDrawBatches;     // 64: first word copied back
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
                                             bool skin_arena, bool meshlet = false) noexcept {
    return (meshlet ? kBatchMeshlet : 0u) | (skinned ? 8u : 0u) | (masked ? 4u : 0u) | (double_sided ? 2u : 0u) |
           (skin_arena ? 1u : 0u);
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
    // ADR-0010: meshlet batches only (the submesh's meshlets; see GpuMeshlet).
    u64  meshlets = 0;       // 48 GpuMeshlet array (device address)
    u64  meshlet_words = 0;  // 56 u32 vertex-index / packed-triangle words of the mesh
    u32  meshlet_count = 0;  // 64
    u32  pad[3]{};           // 68
};
static_assert(sizeof(GpuCullInstance) == 80);
static_assert(offsetof(GpuCullInstance, batch) == 28);
static_assert(offsetof(GpuCullInstance, meshlets) == 48);
static_assert(offsetof(GpuCullInstance, meshlet_count) == 64);

// ---------------------------------------------------------------------------
// Meshlets (ADR-0010). Built per submesh by meshoptimizer at register_mesh(); one GPU buffer per
// mesh holds [GpuMeshlet x n | u32 words]. Words: vertex indices (relative to the mesh's first
// vertex) and triangles packed as i0 | i1 << 8 | i2 << 16 (meshlet-local vertex indices).
// ---------------------------------------------------------------------------
inline constexpr u32 kMeshletMaxVertices = 64;
inline constexpr u32 kMeshletMaxTriangles = 124;
inline constexpr u32 kTaskGroupSize = 32; // meshlets tested per task workgroup

struct GpuMeshlet {
    Vec3 center{ 0.0f };     // 0  bounding sphere (mesh space)
    f32  radius = 0.0f;      // 12
    Vec3 cone_apex{ 0.0f };  // 16 normal cone (mesh space)
    f32  cone_cutoff = 1.0f; // 28 cos(half angle); >= 1 disables cone culling
    Vec3 cone_axis{ 0.0f };  // 32
    u32  vertex_offset = 0;  // 44 first word of the vertex indices
    u32  triangle_offset = 0;// 48 first word of the packed triangles
    u32  vertex_count = 0;   // 52
    u32  triangle_count = 0; // 56
    u32  pad = 0;            // 60
};
static_assert(sizeof(GpuMeshlet) == 64);
static_assert(offsetof(GpuMeshlet, vertex_offset) == 44);

// The 20-byte command slot of a meshlet batch: VkDrawMeshTasksIndirectCommandEXT + the candidate.
struct GpuMeshTaskCommand {
    u32 group_x = 0;         // ceil(meshlet_count / kTaskGroupSize)
    u32 group_y = 1;
    u32 group_z = 1;
    u32 candidate = 0;       // index into the frame's GpuCullInstance array
    u32 pad = 0;
};
static_assert(sizeof(GpuMeshTaskCommand) == 20);

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
static_assert(sizeof(GpuCullView) == 192 + 4 * kMaxDrawBatches);

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

// Task + mesh shaders of meshlet batches (ADR-0010). The first 40 bytes are MeshPush, so the
// fragment shaders (which declare MeshPush) are shared with the vertex path unchanged.
struct MeshletPush {
    u64 frame = 0;            // 0
    u64 light_grid = 0;       // 8
    u64 light_indices = 0;    // 16
    u32 view_index = 0;       // 24
    u32 pass_flags = 0;       // 28
    u32 ssao_tex = kGpuInvalidIndex; // 32
    u32 pad = 0;              // 36
    u64 commands = 0;         // 40 this draw's command array (GpuMeshTaskCommand), indexed by gl_DrawID
    u64 candidates = 0;       // 48 GpuCullInstance array
    u64 vertices = 0;         // 56 static vertex arena (aether::Vertex)
    u32 cone_culling = 1;     // 64 0 for double-sided batches
    u32 pad1 = 0;             // 68
    u64 cull_view = 0;        // 72 GpuCullView (main-view frustum planes)
};
static_assert(sizeof(MeshletPush) == 80);
static_assert(offsetof(MeshletPush, commands) == 40);

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

// ADR-0016: gi_project.comp - one workgroup per probe captured this frame.
struct GiProjectPush {
    u64 frame = 0;       // 0  FrameData (sky / ambient, the capture view matrices)
    u64 probes = 0;      // 8  GiProbeBuffer (written)
    u64 slots = 0;       // 16 u32[] probe index of each capture slot
    u32 capture_tex = 0; // 24 sampler2DArray (RGBA16F, 6 layers per slot; a < 0 = back face, 0 = sky)
    u32 size = 0;        // 28 capture face edge in texels
    u32 view_base = 0;   // 32 GpuSpotShadow entry of slot 0 face 0
    u32 pad = 0;
};
static_assert(sizeof(GiProjectPush) == 40);

// ADR-0017: refl_resolve.comp - the 6 captured faces -> mip 0 of the probe's environment cube.
struct ReflResolvePush {
    u64  frame = 0;       // 0  FrameData (sky, capture view matrices)
    u32  capture_tex = 0; // 8  sampler2DArray (6 layers; a < 0 = back face, 0 = sky)
    u32  dst_img = 0;     // 12 image2DArray (cube mip 0)
    u32  size = 0;        // 16 face edge in texels (capture and cube)
    u32  view_base = 0;   // 20 GpuSpotShadow entry of face 0
    u32  pad0 = 0, pad1 = 0;
    Vec3 position{ 0.0f };// 32 capture point
    f32  pad2 = 0.0f;
};
static_assert(sizeof(ReflResolvePush) == 48);

// ADR-0017: cube_downsample.comp - 2x2 box filter of one cube mip into the next.
struct CubeDownsamplePush {
    u32 src_img = 0;
    u32 dst_img = 0;
    u32 dst_size = 0;
    u32 pad = 0;
};
static_assert(sizeof(CubeDownsamplePush) == 16);

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
