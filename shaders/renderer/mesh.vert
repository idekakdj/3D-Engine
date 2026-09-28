// shaders/renderer/mesh.vert — shared vertex shader for prepass, shadows, forward, overdraw.
// Instance data is fetched with gl_InstanceIndex (= firstInstance of the draw). With
// AE_SKINNED the SkinVertex stream is read through a buffer device address indexed by
// gl_VertexIndex (the skinned arena keeps both streams in lockstep) and the palette is
// frame.joints[joint_offset + joint] (skinning matrices in model space).
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "mesh_push.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec2 v_uv;
layout(location = 4) flat out uint v_instance;

// Prepass and forward must produce bit-identical depth (the forward pass tests Equal).
invariant gl_Position;

void main() {
    FrameData   frame = pc.frame;
    GpuInstance inst = frame.instances.items[gl_InstanceIndex];

    vec4 local_pos = vec4(in_position, 1.0);
    vec3 local_n = in_normal;
    vec3 local_t = in_tangent.xyz;
#ifdef AE_SKINNED
    if (inst.joint_count > 0u) {
        GpuSkinVertex sv = frame.skin.items[gl_VertexIndex];
        uvec4 j = uvec4(sv.joints01 & 0xFFFFu, sv.joints01 >> 16, sv.joints23 & 0xFFFFu, sv.joints23 >> 16);
        j = min(j, uvec4(inst.joint_count - 1u));
        vec4  w = sv.weights;
        float wsum = w.x + w.y + w.z + w.w;
        w = wsum > 1e-6 ? w / wsum : vec4(1.0, 0.0, 0.0, 0.0);
        uint base = inst.joint_offset;
        mat4 skin = frame.joints.items[base + j.x] * w.x + frame.joints.items[base + j.y] * w.y +
                    frame.joints.items[base + j.z] * w.z + frame.joints.items[base + j.w] * w.w;
        local_pos = skin * local_pos;
        mat3 skin3 = mat3(skin); // joints assumed free of non-uniform scale
        local_n = skin3 * local_n;
        local_t = skin3 * local_t;
    }
#endif
    vec4 world = inst.model * local_pos;
    mat3 normal_matrix = mat3(inst.normal_col0.xyz, inst.normal_col1.xyz, inst.normal_col2.xyz);

    v_world_pos = world.xyz;
    v_normal = ae_safe_normalize(normal_matrix * local_n);
    v_tangent = vec4(ae_safe_normalize(mat3(inst.model) * local_t), in_tangent.w < 0.0 ? -1.0 : 1.0);
    v_uv = in_uv;
    v_instance = uint(gl_InstanceIndex);

    // 0 = camera, 1 + i = shadow cascade i, 1 + AE_MAX_CASCADES + j = local shadow view j (spot ADR-0012 / point face ADR-0015).
    mat4 view_proj = pc.view_index == 0u                          ? frame.view_proj
                     : pc.view_index <= uint(AE_MAX_CASCADES)     ? frame.cascade_view_proj[pc.view_index - 1u]
                                                                  : frame.spot_shadows.items[pc.view_index - 1u - uint(AE_MAX_CASCADES)].view_proj;
    gl_Position = view_proj * world;
}
