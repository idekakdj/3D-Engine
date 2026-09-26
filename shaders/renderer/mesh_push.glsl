// shaders/renderer/mesh_push.glsl — push constants for every mesh pass (mirrors MeshPush).
#ifndef AE_MESH_PUSH_GLSL
#define AE_MESH_PUSH_GLSL

layout(push_constant, scalar) uniform MeshPush {
    FrameData      frame;
    ClusterGrid    light_grid;    // forward only
    ClusterIndices light_indices; // forward only
    uint           view_index;    // 0 = camera, 1 + i = shadow cascade i
    uint           pass_flags;
    uint           ssao_tex;
    uint           pad;
} pc;

#endif
