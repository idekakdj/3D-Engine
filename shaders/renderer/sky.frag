// shaders/renderer/sky.frag — environment cubemap (or flat ambient colour) behind the scene.
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"

layout(push_constant, scalar) uniform SkyPush {
    FrameData frame;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    FrameData frame = pc.frame;
    vec2 ndc = gl_FragCoord.xy * frame.inv_viewport * 2.0 - 1.0;
    vec4 p = frame.inv_view_proj * vec4(ndc, 1.0, 1.0); // reverse-Z: depth 1 = near plane
    vec3 dir = ae_safe_normalize(p.xyz / p.w - frame.camera_pos);
    vec3 c = frame.skybox_map != AE_INVALID_INDEX
                 ? textureLod(AE_TEXCUBE(frame.skybox_map), dir, frame.skybox_lod).rgb * frame.ibl_intensity
                 : frame.ambient;
    if (frame.debug_view != AE_DEBUG_NONE) {
        c = vec3(0.0);
    }
    out_color = vec4(c, 1.0);
}
