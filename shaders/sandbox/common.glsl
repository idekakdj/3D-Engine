// shaders/sandbox/common.glsl — push-constant block shared by every sandbox shader
// (the universal layout has one 128-byte range; all stages see the same block).
#ifndef SANDBOX_COMMON_GLSL
#define SANDBOX_COMMON_GLSL

#include "common/bindless.glsl"

struct TriVertex {
    vec2 pos;
    vec2 uv;
    vec3 color;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TriVertices {
    TriVertex v[];
};

// Mirrors `SandboxPush` in samples/sandbox/main.cpp (scalar layout, 32 bytes).
layout(push_constant, scalar) uniform SandboxPush {
    TriVertices vertices;   // buffer device address of the triangle's vertices
    float       angle;      // rotation (radians)
    float       aspect;     // framebuffer width / height
    uint        checker_tex; // bindless sampled index (mipmapped checkerboard)
    uint        pattern_tex; // bindless sampled index (compute-generated pattern)
    uint        pattern_img; // bindless storage index (same image, written by compute)
    float       time;
} pc;

#endif
