// shaders/renderer/debug_line.frag — depth test against the scene depth in-shader
// (reverse-Z: visible when fragment depth >= scene depth), so lines work at any output
// resolution without a depth attachment. Colours are sRGB-authored (display values).
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "color.glsl"

layout(push_constant, scalar) uniform LinePush {
    FrameData frame;
    uint      depth_tex;
    uint      decode_srgb;
    vec2      inv_target_size;
    float     depth_bias;
    uint      pad;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    if (pc.depth_tex != AE_INVALID_INDEX) {
        float scene = textureLod(AE_TEX2D(pc.depth_tex), gl_FragCoord.xy * pc.inv_target_size, 0.0).r;
        if (gl_FragCoord.z + pc.depth_bias < scene) {
            discard;
        }
    }
    vec4 c = v_color;
    if (pc.decode_srgb != 0u) {
        c.rgb = ae_srgb_to_linear(c.rgb);
    }
    out_color = c;
}
