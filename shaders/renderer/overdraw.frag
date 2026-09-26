// shaders/renderer/overdraw.frag — Overdraw debug view: +1 per shaded fragment
// (additive blend, depth test off). tonemap.frag maps the count to a heat ramp.
#version 460

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(1.0, 1.0, 1.0, 1.0);
}
