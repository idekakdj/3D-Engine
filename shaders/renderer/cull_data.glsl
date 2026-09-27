// shaders/renderer/cull_data.glsl — GPU-driven culling data (ADR-0009). GLSL side of the
// "GPU-driven culling + picking" block of engine/renderer/src/gpu_data.h (scalar layout).
// Include after "common/bindless.glsl".
#ifndef AE_CULL_DATA_GLSL
#define AE_CULL_DATA_GLSL

#ifndef AE_MAX_DRAW_BATCHES
#define AE_MAX_DRAW_BATCHES 32
#endif
#define AE_BATCH_MESHLET 16u
#ifndef AE_TASK_GROUP_SIZE
#define AE_TASK_GROUP_SIZE 32
#endif

#define AE_CULL_NEVER_CULL 1u

// Counter buffer words (kCounter* in gpu_data.h).
#define AE_COUNTER_PHASE1           0u
#define AE_COUNTER_PHASE2           uint(AE_MAX_DRAW_BATCHES)
#define AE_COUNTER_STATS            uint(2 * AE_MAX_DRAW_BATCHES)
#define AE_COUNTER_FRUSTUM_CULLED   (AE_COUNTER_STATS + 0u)
#define AE_COUNTER_OCCLUSION_CULLED (AE_COUNTER_STATS + 1u)
#define AE_COUNTER_VISIBLE          (AE_COUNTER_STATS + 2u)
#define AE_COUNTER_TRIANGLES        (AE_COUNTER_STATS + 3u)
#define AE_COUNTER_PICK             (AE_COUNTER_STATS + 4u)

#define AE_CULL_PHASE_FRUSTUM 0u
#define AE_CULL_PHASE_1       1u
#define AE_CULL_PHASE_2       2u

// Meshlets (ADR-0010; gpu_data.h GpuMeshlet): per-mesh arrays addressed per candidate.
struct GpuMeshlet {
    vec3  center;
    float radius;
    vec3  cone_apex;
    float cone_cutoff;
    vec3  cone_axis;
    uint  vertex_offset;
    uint  triangle_offset;
    uint  vertex_count;
    uint  triangle_count;
    uint  pad;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MeshletBuffer { GpuMeshlet items[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer WordBuffer { uint words[]; };

struct GpuCullInstance {
    vec3 aabb_min;
    uint instance;
    vec3 aabb_max;
    uint batch;
    uint first_index;
    uint index_count;
    int  vertex_offset;
    uint flags;
    MeshletBuffer meshlets;   // ADR-0010: meshlet batches only
    WordBuffer    meshlet_words;
    uint meshlet_count;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct GpuMeshTaskCommand { // VkDrawMeshTasksIndirectCommandEXT + candidate
    uint group_x;
    uint group_y;
    uint group_z;
    uint candidate;
    uint pad;
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
layout(buffer_reference, scalar, buffer_reference_align = 4) writeonly buffer MeshTaskCommandBuffer { GpuMeshTaskCommand cmds[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer MeshTaskCommandReadBuffer { GpuMeshTaskCommand cmds[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer CounterBuffer { uint values[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer VisibilityBuffer { uint values[]; };

#endif // AE_CULL_DATA_GLSL
