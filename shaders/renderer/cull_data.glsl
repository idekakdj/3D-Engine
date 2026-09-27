// shaders/renderer/cull_data.glsl — GPU-driven culling data (ADR-0009). GLSL side of the
// "GPU-driven culling + picking" block of engine/renderer/src/gpu_data.h (scalar layout).
// Include after "common/bindless.glsl".
#ifndef AE_CULL_DATA_GLSL
#define AE_CULL_DATA_GLSL

#ifndef AE_MAX_DRAW_BATCHES
#define AE_MAX_DRAW_BATCHES 16
#endif

#define AE_CULL_NEVER_CULL 1u

// Counter buffer words (kCounter* in gpu_data.h).
#define AE_COUNTER_PHASE1           0u
#define AE_COUNTER_PHASE2           16u
#define AE_COUNTER_FRUSTUM_CULLED   32u
#define AE_COUNTER_OCCLUSION_CULLED 33u
#define AE_COUNTER_VISIBLE          34u
#define AE_COUNTER_TRIANGLES        35u
#define AE_COUNTER_PICK             36u

#define AE_CULL_PHASE_FRUSTUM 0u
#define AE_CULL_PHASE_1       1u
#define AE_CULL_PHASE_2       2u

struct GpuCullInstance {
    vec3 aabb_min;
    uint instance;
    vec3 aabb_max;
    uint batch;
    uint first_index;
    uint index_count;
    int  vertex_offset;
    uint flags;
};

struct GpuDrawCommand { // VkDrawIndexedIndirectCommand
    uint index_count;
    uint instance_count;
    uint first_index;
    int  vertex_offset;
    uint first_instance;
};

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer CullInstanceBuffer { GpuCullInstance items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer CullViewBuffer {
    vec4  planes[6];
    mat4  view_proj;   // jittered (matches the depth buffer)
    vec2  viewport;    // depth-buffer extent (pixels)
    uvec2 hzb_size;    // Hi-Z mip 0 extent
    uint  hzb_mips;
    uint  hzb_tex;     // sampler2D, texelFetch per mip
    uint  plane_count;
    uint  pad0;
    uint  batch_offset[AE_MAX_DRAW_BATCHES];
};
layout(buffer_reference, scalar, buffer_reference_align = 4) writeonly buffer DrawCommandBuffer { GpuDrawCommand cmds[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer CounterBuffer { uint values[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer VisibilityBuffer { uint values[]; };

#endif // AE_CULL_DATA_GLSL
