// shaders/renderer/depth.frag — depth prepass / shadow caster fragment shader.
// Opaque: empty (depth only). AE_ALPHA_MASK: alpha test against the material cutoff.
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "mesh_push.glsl"

layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_instance;

void main() {
#ifdef AE_ALPHA_MASK
    FrameData   frame = pc.frame;
    GpuMaterial m = frame.materials.items[frame.instances.items[v_instance].material];
    float alpha = m.base_color.a * ae_sample_or(m.base_color_tex, v_uv, vec4(1.0)).a;
    if (alpha < m.alpha_cutoff) {
        discard;
    }
#endif
}
