// shaders/renderer/sky.vert — fullscreen triangle at the reverse-Z far plane (depth 0);
// with GreaterEqual and no depth writes it only covers pixels the scene left empty.
#version 460

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
