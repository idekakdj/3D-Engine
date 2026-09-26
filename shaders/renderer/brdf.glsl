// shaders/renderer/brdf.glsl — Cook-Torrance GGX BRDF + punctual light helpers.
// Conventions: `alpha` = perceptual_roughness^2; directions are unit vectors.
#ifndef AE_BRDF_GLSL
#define AE_BRDF_GLSL

float ae_d_ggx(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (AE_PI * f * f + 1e-7);
}

// Height-correlated Smith visibility (Heitz 2014), V = G / (4 NoL NoV).
float ae_v_smith_ggx_correlated(float NoV, float NoL, float alpha) {
    float a2 = alpha * alpha;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-6);
}

vec3 ae_f_schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

// Inverse-square falloff with a smooth window reaching 0 at `range` (Karis 2013).
float ae_distance_attenuation(float d, float range) {
    float d2 = d * d;
    float ratio = d2 / max(range * range, 1e-8);
    float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
    return (window * window) / max(d2, 1e-4);
}

// Analytic split-sum DFG fallback (Karis, mobile) when the LUT is unavailable. x=A, y=B.
vec2 ae_dfg_approx(float NoV, float perceptual_roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = perceptual_roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Specular occlusion from ambient occlusion (Lagarde & de Rousiers 2014).
float ae_specular_occlusion(float NoV, float ao, float alpha) {
    return clamp(pow(NoV + ao, exp2(-16.0 * alpha - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// Direct lighting for one light: (energy-conserving Lambert + GGX specular) * NoL.
vec3 ae_evaluate_brdf(vec3 N, vec3 V, vec3 L, vec3 diffuse_color, vec3 f0, float alpha,
                      vec3 energy_comp) {
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    if (NoL <= 0.0) {
        return vec3(0.0);
    }
    vec3  H = ae_safe_normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    vec3  F = ae_f_schlick(f0, VoH);
    vec3  spec = ae_d_ggx(NoH, alpha) * ae_v_smith_ggx_correlated(NoV, NoL, alpha) * F * energy_comp;
    vec3  diff = diffuse_color * (1.0 / AE_PI) * (1.0 - F);
    return (diff + spec) * NoL;
}

#endif
