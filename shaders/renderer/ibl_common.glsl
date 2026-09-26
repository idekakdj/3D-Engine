// shaders/renderer/ibl_common.glsl — shared IBL precompute helpers (push block, cube
// face directions, Hammersley, GGX importance sampling, filtered-importance LOD).
#ifndef AE_IBL_COMMON_GLSL
#define AE_IBL_COMMON_GLSL

#define AE_IBL_PI 3.14159265358979

layout(push_constant, scalar) uniform IblPush {
    uint  src_tex;
    uint  dst_img;
    uint  dst_size;
    uint  sample_count;
    float roughness;
    float src_lod;
    uint  src_size;
    uint  src_mips;
} pc;

// Direction through texel `uv` (0..1) of cube face `face` (+X, -X, +Y, -Y, +Z, -Z), using
// the Vulkan cube-map face orientation so samplerCube lookups match what we write.
vec3 ibl_cube_dir(uint face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    vec3 d;
    switch (face) {
    case 0u: d = vec3(1.0, -st.y, -st.x); break;
    case 1u: d = vec3(-1.0, -st.y, st.x); break;
    case 2u: d = vec3(st.x, 1.0, st.y); break;
    case 3u: d = vec3(st.x, -1.0, -st.y); break;
    case 4u: d = vec3(st.x, -st.y, 1.0); break;
    default: d = vec3(-st.x, -st.y, -1.0); break;
    }
    return normalize(d);
}

vec2 ibl_hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10);
}

mat3 ibl_basis(vec3 n) {
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    return mat3(t, cross(n, t), n);
}

// GGX-distributed half vector around N (alpha = roughness^2).
vec3 ibl_importance_sample_ggx(vec2 xi, float alpha, vec3 n) {
    float phi = 2.0 * AE_IBL_PI * xi.x;
    float cos_t = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
    return ibl_basis(n) * vec3(cos(phi) * sin_t, sin(phi) * sin_t, cos_t);
}

float ibl_d_ggx(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (AE_IBL_PI * f * f + 1e-7);
}

// Filtered importance sampling (Krivanek & Colbert): source mip whose texel solid angle
// matches the sample's solid angle.
float ibl_source_lod(float pdf, uint sample_count) {
    float sa_sample = 1.0 / (float(sample_count) * max(pdf, 1e-6));
    float sa_texel = 4.0 * AE_IBL_PI / (6.0 * float(pc.src_size) * float(pc.src_size));
    return clamp(0.5 * log2(sa_sample / sa_texel) + 1.0, 0.0, float(pc.src_mips - 1u));
}

#endif
