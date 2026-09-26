// shaders/renderer/color.glsl — colour-space helpers (sRGB, YCoCg, ACES, heat map).
#ifndef AE_COLOR_GLSL
#define AE_COLOR_GLSL

float ae_luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 ae_linear_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 ae_srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 ae_rgb_to_ycocg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 ae_ycocg_to_rgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// ACES fitted (Stephen Hill): sRGB -> AP1 RRT+ODT fit -> sRGB, linear output.
vec3 ae_aces_fitted(vec3 color) {
    const mat3 aces_in = mat3(0.59719, 0.07600, 0.02840,
                              0.35458, 0.90834, 0.13383,
                              0.04823, 0.01566, 0.83777);
    const mat3 aces_out = mat3(1.60475, -0.10208, -0.00327,
                               -0.53108, 1.10813, -0.07276,
                               -0.07367, -0.00605, 1.07602);
    vec3 v = aces_in * color;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return clamp(aces_out * (a / b), 0.0, 1.0);
}

// Blue -> green -> yellow -> red ramp for t in [0, 1] (>1 saturates to magenta).
vec3 ae_heatmap(float t) {
    if (t > 1.0) {
        return vec3(1.0, 0.0, 1.0);
    }
    vec3 c = vec3(clamp(t * 4.0 - 2.0, 0.0, 1.0), clamp(t < 0.5 ? t * 4.0 : 4.0 - t * 4.0, 0.0, 1.0),
                  clamp(2.0 - t * 4.0, 0.0, 1.0));
    return c;
}

// Interleaved gradient noise (Jimenez 2014).
float ae_ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

#endif
