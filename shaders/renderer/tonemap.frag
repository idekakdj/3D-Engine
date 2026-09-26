// shaders/renderer/tonemap.frag — exposure, bloom composite, ACES fitted, output encoding.
// Encoding rule (ADR-0002): apply_oetf = 1 for *Unorm targets (sRGB OETF in-shader),
// 0 for *Srgb (hardware encodes) and float targets (linear). Debug views bypass exposure,
// bloom and ACES. Reads the internal-resolution image with bilinear filtering, so the
// target extent may differ from the internal render resolution.
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "color.glsl"

layout(push_constant, scalar) uniform TonemapPush {
    uint  color_tex;
    uint  bloom_tex;
    float exposure;
    float bloom_intensity;
    uint  debug_view;
    uint  apply_oetf;
    vec2  inv_target_size;
    float bloom_norm;
    uint  pad;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec2 uv = gl_FragCoord.xy * pc.inv_target_size;
    vec3 c = textureLod(AE_TEX2D(pc.color_tex), uv, 0.0).rgb;
    if (pc.debug_view == AE_DEBUG_OVERDRAW) {
        c = ae_heatmap(c.r / 16.0);
    } else if (pc.debug_view != AE_DEBUG_NONE) {
        c = clamp(c, 0.0, 1.0);
    } else {
        if (pc.bloom_tex != AE_INVALID_INDEX) {
            vec3 bloom = textureLod(AE_TEX2D(pc.bloom_tex), uv, 0.0).rgb * pc.bloom_norm;
            c = mix(c, bloom, pc.bloom_intensity);
        }
        c = ae_aces_fitted(max(c, vec3(0.0)) * pc.exposure);
    }
    if (pc.apply_oetf != 0u) {
        c = ae_linear_to_srgb(c);
        // +-0.5 LSB triangular-ish dither against 8-bit banding.
        c += (ae_ign(gl_FragCoord.xy) - 0.5) / 255.0;
    }
    out_color = vec4(c, 1.0);
}
