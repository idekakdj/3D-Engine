// shaders/renderer/gi_capture.frag — one cube face of a GI probe capture (ADR-0016).
// Outgoing diffuse radiance of the visible surface: Lambert albedo x (direct light from every
// light with its shadow maps + the current GI volume = the previous bounces) + emissive. Alpha
// marks what the texel saw for gi_project.comp: 1 = front face, -1 = back face of single-sided
// geometry (the probe is inside something), cleared 0 = nothing (sky / ambient).
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "mesh_push.glsl"
#include "brdf.glsl"
#include "shadows.glsl"
#include "gi.glsl"

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_instance;

layout(location = 0) out vec4 out_color;

// Lights evaluated per capture texel (the capture has no cluster grid).
#define AE_GI_MAX_CAPTURE_LIGHTS 64u

void main() {
    FrameData   frame = pc.frame;
    GpuInstance inst = frame.instances.items[v_instance];
    GpuMaterial m = frame.materials.items[inst.material];

    vec4 base = m.base_color * ae_sample_or(m.base_color_tex, v_uv, vec4(1.0));
    if ((m.flags & AE_MATERIAL_MASKED) != 0u && base.a < m.alpha_cutoff) {
        discard;
    }
    bool double_sided = (m.flags & AE_MATERIAL_DOUBLE_SIDED) != 0u;
    if (!gl_FrontFacing && !double_sided) {
        out_color = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }
    float metallic = m.metallic;
    if (m.metallic_roughness_tex != AE_INVALID_INDEX) {
        metallic *= texture(AE_TEX2D(m.metallic_roughness_tex), v_uv).b;
    }
    vec3 albedo = base.rgb * (1.0 - clamp(metallic, 0.0, 1.0));
    vec3 emissive = m.emissive * ae_sample_or(m.emissive_tex, v_uv, vec4(1.0)).rgb;

    vec3 Ng = ae_safe_normalize(v_normal);
    if (!gl_FrontFacing) {
        Ng = -Ng;
    }
    // Sun shadows are fitted to the main camera: their cascade is picked from its view depth.
    float view_z = -(frame.view * vec4(v_world_pos, 1.0)).z;

    vec3 irradiance = vec3(0.0); // sum of light colour x cos x attenuation x shadow
    uint n = min(frame.light_count, frame.directional_count + AE_GI_MAX_CAPTURE_LIGHTS);
    for (uint i = 0u; i < n; ++i) {
        GpuLight l = frame.lights.items[i];
        vec3     L;
        float    atten = 1.0;
        if (l.type == AE_LIGHT_DIRECTIONAL) {
            L = -l.direction;
            if (l.shadow != AE_INVALID_INDEX && (frame.flags & AE_FRAME_SHADOWS) != 0u) {
                atten = ae_cascaded_shadow(frame, v_world_pos, Ng, L, view_z);
            }
        } else {
            vec3  to_light = l.position - v_world_pos;
            float d = length(to_light);
            L = to_light / max(d, 1e-4);
            atten = ae_distance_attenuation(d, l.range);
            if (l.type == AE_LIGHT_SPOT) {
                float s = clamp(dot(-L, l.direction) * l.spot_scale + l.spot_offset, 0.0, 1.0);
                atten *= s * s;
                if (atten > 0.0 && l.shadow != AE_INVALID_INDEX) {
                    atten *= ae_spot_shadow(frame, l.shadow, v_world_pos, Ng, L, d);
                }
            } else if (atten > 0.0 && l.shadow != AE_INVALID_INDEX) {
                atten *= ae_point_shadow(frame, l.shadow, v_world_pos, Ng, L, d);
            }
        }
        irradiance += l.color * (max(dot(Ng, L), 0.0) * atten);
    }
    vec3 radiance = albedo * irradiance * (1.0 / AE_PI);

    // Previous bounces: the GI volume itself (the environment where it has no data).
    float gi_w;
    vec3  gi = ae_gi_irradiance(frame, v_world_pos, Ng, Ng, gi_w);
    vec3  env = (frame.flags & AE_FRAME_IBL) != 0u
                    ? texture(AE_TEXCUBE(frame.irradiance_map), Ng).rgb * frame.ibl_intensity
                    : frame.ambient;
    radiance += albedo * mix(env, gi, gi_w);
    radiance += emissive;
    out_color = vec4(radiance, 1.0);
}
