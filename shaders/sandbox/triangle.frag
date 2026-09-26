#version 460
// Samples two bindless textures: a mipmapped checkerboard and the compute-written pattern.
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 checker = ae_sample_or(pc.checker_tex, v_uv * 6.0, vec4(1.0)).rgb;
    vec3 pattern = ae_sample_or(pc.pattern_tex, v_uv, vec4(0.5)).rgb;
    o_color = vec4(v_color * mix(checker, pattern, 0.45), 1.0);
}
