// shaders/renderer/debug_line.vert — 1 px LINE_LIST debug lines (Intel Arc: no wideLines).
// Vertices are pulled from the frame's line buffer (2 per RenderLine).
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"

layout(push_constant, scalar) uniform LinePush {
    FrameData frame;
    uint      depth_tex;
    uint      decode_srgb;
    vec2      inv_target_size;
    float     depth_bias;
    uint      pad;
} pc;

layout(location = 0) out vec4 v_color;

void main() {
    GpuLineVertex v = pc.frame.lines.items[gl_VertexIndex];
    v_color = unpackUnorm4x8(v.color);
    gl_Position = pc.frame.unjittered_view_proj * vec4(v.position, 1.0);
}
