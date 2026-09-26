#version 460
// Background quad using classic vertex input (binding 0: vec3 position, vec3 color).
#include "common.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 0) out vec3 v_color;

void main() {
    gl_Position = vec4(a_position.x / pc.aspect, a_position.y, a_position.z, 1.0);
    v_color     = a_color;
}
