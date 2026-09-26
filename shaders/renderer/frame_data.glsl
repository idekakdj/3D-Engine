// shaders/renderer/frame_data.glsl — GLSL side of engine/renderer/src/gpu_data.h.
// Scalar block layout; every offset is pinned by static_asserts on the C++ side.
// Include after "common/bindless.glsl" (which enables the buffer_reference extensions).
#ifndef AE_FRAME_DATA_GLSL
#define AE_FRAME_DATA_GLSL

// Shared constants (the renderer injects these as defines; defaults keep files standalone).
#ifndef AE_CLUSTER_X
#define AE_CLUSTER_X 16
#endif
#ifndef AE_CLUSTER_Y
#define AE_CLUSTER_Y 9
#endif
#ifndef AE_CLUSTER_Z
#define AE_CLUSTER_Z 24
#endif
#ifndef AE_MAX_LIGHTS_PER_CLUSTER
#define AE_MAX_LIGHTS_PER_CLUSTER 128
#endif
#ifndef AE_MAX_CASCADES
#define AE_MAX_CASCADES 4
#endif

#define AE_PI 3.14159265358979

#define AE_LIGHT_DIRECTIONAL 0u
#define AE_LIGHT_POINT       1u
#define AE_LIGHT_SPOT        2u

#define AE_MATERIAL_MASKED       1u
#define AE_MATERIAL_TRANSLUCENT  2u
#define AE_MATERIAL_DOUBLE_SIDED 4u

#define AE_FRAME_SHADOWS 1u
#define AE_FRAME_IBL     2u

#define AE_PASS_SHADOW   1u
#define AE_PASS_OVERDRAW 2u

// renderer::DebugView
#define AE_DEBUG_NONE             0u
#define AE_DEBUG_ALBEDO           1u
#define AE_DEBUG_NORMALS          2u
#define AE_DEBUG_ROUGHNESS        3u
#define AE_DEBUG_METALLIC         4u
#define AE_DEBUG_AO               5u
#define AE_DEBUG_EMISSIVE         6u
#define AE_DEBUG_LIGHT_COMPLEXITY 7u
#define AE_DEBUG_SHADOW_CASCADES  8u
#define AE_DEBUG_OVERDRAW         9u

struct GpuInstance {
    mat4 model;
    vec4 normal_col0; // inverse-transpose(model) columns
    vec4 normal_col1;
    vec4 normal_col2;
    uint material;
    uint flags;
    uint joint_offset;
    uint joint_count;
};

struct GpuMaterial {
    vec4  base_color;
    vec3  emissive;
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
    uint  base_color_tex;
    uint  metallic_roughness_tex;
    uint  normal_tex;
    uint  occlusion_tex;
    uint  emissive_tex;
    uint  flags;
    uint  pad0;
    uint  pad1;
};

struct GpuLight {
    vec3  position;
    float range;
    vec3  direction; // direction the light travels
    uint  type;
    vec3  color;     // color * intensity
    float spot_scale;
    float spot_offset;
    uint  shadow;
    uint  pad0;
    uint  pad1;
};

struct GpuLineVertex {
    vec3 position;
    uint color; // RGBA8, R in the low byte
};

struct GpuSkinVertex {
    uint joints01; // joint0 | joint1 << 16
    uint joints23;
    vec4 weights;
};

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer InstanceBuffer { GpuInstance items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MaterialBuffer { GpuMaterial items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LightBuffer { GpuLight items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer JointBuffer { mat4 items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LineBuffer { GpuLineVertex items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer SkinBuffer { GpuSkinVertex items[]; };

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer ClusterGrid { uint counts[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer ClusterIndices { uint indices[]; };

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FrameData {
    InstanceBuffer instances;
    MaterialBuffer materials;
    LightBuffer    lights;
    JointBuffer    joints;
    LineBuffer     lines;
    SkinBuffer     skin;
    mat4  view;
    mat4  proj;          // jittered, reverse-Z
    mat4  view_proj;     // jittered
    mat4  inv_view;
    mat4  inv_proj;      // jittered
    mat4  inv_view_proj; // jittered
    mat4  unjittered_view_proj;
    mat4  prev_view_proj;
    mat4  cascade_view_proj[AE_MAX_CASCADES];
    vec4  cascade_splits;
    vec4  cascade_texel_world;
    vec3  camera_pos;
    float near_z;
    vec2  viewport;
    vec2  inv_viewport;
    vec2  jitter_ndc;
    float far_z;
    float exposure;
    vec3  ambient;
    float ibl_intensity;
    uint  light_count;
    uint  directional_count;
    uint  shadow_map;
    uint  cascade_count;
    uint  irradiance_map;
    uint  prefiltered_map;
    uint  brdf_lut;
    uint  prefiltered_mips;
    uint  skybox_map;
    float skybox_lod;
    uint  debug_view;
    uint  flags;
    uint  cluster_x;
    uint  cluster_y;
    uint  cluster_z;
    float cluster_z_scale;
    float cluster_z_bias;
    uint  cluster_tile_w;
    uint  cluster_tile_h;
    uint  frame_index;
    float shadow_distance;
    float shadow_fade_start;
    float shadow_normal_bias;
    float cascade_blend;
};

vec3 ae_safe_normalize(vec3 v) { return v * inversesqrt(max(dot(v, v), 1e-12)); }

#endif // AE_FRAME_DATA_GLSL
