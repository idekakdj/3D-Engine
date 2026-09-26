#version 460
// Rotating triangle: vertices are pulled through a buffer device address (no vertex input).
#include "common.glsl"

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_color;

void main() {
    TriVertex vtx = pc.vertices.v[gl_VertexIndex];
    float c = cos(pc.angle), s = sin(pc.angle);
    vec2  p = mat2(c, s, -s, c) * vtx.pos;
    gl_Position = vec4(p.x / pc.aspect, p.y, 0.5, 1.0); // reverse-Z: 0.5 is in front of the quad
    v_uv    = vtx.uv;
    v_color = vtx.color;
}
