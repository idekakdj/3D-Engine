// shaders/renderer/meshlet_push.glsl — push constants of the meshlet task/mesh shaders (mirrors
// MeshletPush, ADR-0010). Its first 40 bytes are MeshPush, so the fragment shaders of the vertex
// path (which declare MeshPush) run unchanged behind the mesh shader.
// Include after "frame_data.glsl" and "cull_data.glsl".
#ifndef AE_MESHLET_PUSH_GLSL
#define AE_MESHLET_PUSH_GLSL

struct GpuVertex { // aether::Vertex (48 bytes)
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer VertexBuffer { GpuVertex items[]; };

layout(push_constant, scalar) uniform MeshletPush {
    FrameData                 frame;
    ClusterGrid               light_grid;
    ClusterIndices            light_indices;
    uint                      view_index;
    uint                      pass_flags;
    uint                      ssao_tex;
    uint                      pad;
    MeshTaskCommandReadBuffer commands;   // this draw's commands, indexed by gl_DrawID
    CullInstanceBuffer        candidates;
    VertexBuffer              vertices;   // static vertex arena
    uint                      cone_culling;
    uint                      pad1;
    CullViewBuffer            cull_view;  // main-view frustum planes
} pc;

// Task -> mesh payload: the surviving meshlets of one task workgroup.
struct TaskPayload {
    uint candidate;
    uint meshlets[AE_TASK_GROUP_SIZE];
};

#endif
