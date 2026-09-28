// shaders/renderer/forward.frag — clustered forward+ PBR (opaque/masked; AE_TRANSLUCENT for
// sorted alpha-blended geometry).
//   Cook-Torrance: GGX D, height-correlated Smith V, Schlick F, multiscatter energy
//   compensation; energy-conserving Lambert diffuse; tangent-space normal maps (bitangent
//   sign in tangent.w); occlusion; emissive; directional (CSM) + clustered point/spot
//   lights with inverse-square windowed falloff; IBL (irradiance + split-sum specular) or
//   flat ambient, replaced inside a GI probe volume by its irradiance (ADR-0016); SSAO on
//   indirect light; debug views.
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "mesh_push.glsl"
#include "color.glsl"
#include "brdf.glsl"
#include "shadows.glsl"
#include "clusters.glsl"
#include "gi.glsl"
#include "reflections.glsl"

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_instance;

layout(location = 0) out vec4 out_color;

vec3 cascade_tint(uint c) {
    const vec3 tints[4] = vec3[4](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35), vec3(0.35, 0.35, 1.0),
                                  vec3(1.0, 1.0, 0.35));
    return tints[min(c, 3u)];
}

void main() {
    FrameData   frame = pc.frame;
    GpuInstance inst = frame.instances.items[v_instance];
    GpuMaterial m = frame.materials.items[inst.material];

    // ---- material inputs ----
    vec4 base = m.base_color * ae_sample_or(m.base_color_tex, v_uv, vec4(1.0));
    float perceptual_roughness = m.roughness;
    float metallic = m.metallic;
    if (m.metallic_roughness_tex != AE_INVALID_INDEX) {
        vec4 mr = texture(AE_TEX2D(m.metallic_roughness_tex), v_uv); // glTF: G = rough, B = metal
        perceptual_roughness *= mr.g;
        metallic *= mr.b;
    }
    perceptual_roughness = clamp(perceptual_roughness, 0.045, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    float alpha = perceptual_roughness * perceptual_roughness;

    float material_ao = 1.0;
    if (m.occlusion_tex != AE_INVALID_INDEX) {
        material_ao = 1.0 + m.occlusion_strength * (texture(AE_TEX2D(m.occlusion_tex), v_uv).r - 1.0);
    }
    vec3 emissive = m.emissive * ae_sample_or(m.emissive_tex, v_uv, vec4(1.0)).rgb;

    // ---- shading frame (double-sided: flip the whole TBN for back faces) ----
    vec3  Ng = ae_safe_normalize(v_normal);
    vec3  T = v_tangent.xyz - Ng * dot(Ng, v_tangent.xyz);
    T = ae_safe_normalize(T);
    vec3  B = cross(Ng, T) * v_tangent.w;
    if (!gl_FrontFacing) {
        Ng = -Ng;
        T = -T;
        B = -B;
    }
    vec3 N = Ng;
    if (m.normal_tex != AE_INVALID_INDEX) {
        vec3 tn = texture(AE_TEX2D(m.normal_tex), v_uv).xyz * 2.0 - 1.0;
        tn.xy *= m.normal_scale;
        N = ae_safe_normalize(mat3(T, B, Ng) * tn);
    }
    vec3  V = ae_safe_normalize(frame.camera_pos - v_world_pos);
    float NoV = max(dot(N, V), 1e-4);

    vec3 diffuse_color = base.rgb * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
    vec2 dfg = frame.brdf_lut != AE_INVALID_INDEX
                   ? textureLod(AE_TEX2D(frame.brdf_lut), vec2(NoV, perceptual_roughness), 0.0).rg
                   : ae_dfg_approx(NoV, perceptual_roughness);
    vec3 energy_comp = 1.0 + f0 * (1.0 / max(dfg.x + dfg.y, 1e-4) - 1.0);

    float view_z = -(frame.view * vec4(v_world_pos, 1.0)).z;

    // ---- directional lights (global) ----
    vec3  color = vec3(0.0);
    float sun_shadow = 1.0;
    for (uint i = 0u; i < frame.directional_count; ++i) {
        GpuLight l = frame.lights.items[i];
        vec3     L = -l.direction;
        float    shadow = 1.0;
        if (l.shadow != AE_INVALID_INDEX && (frame.flags & AE_FRAME_SHADOWS) != 0u) {
            shadow = ae_cascaded_shadow(frame, v_world_pos, Ng, L, view_z);
            sun_shadow = shadow;
        }
        color += ae_evaluate_brdf(N, V, L, diffuse_color, f0, alpha, energy_comp) * l.color * shadow;
    }

    // ---- clustered point / spot lights ----
    uint cluster = ae_cluster_for_fragment(frame, gl_FragCoord.xy, view_z);
    uint count = min(pc.light_grid.counts[cluster], uint(AE_MAX_LIGHTS_PER_CLUSTER));
    uint list_base = cluster * uint(AE_MAX_LIGHTS_PER_CLUSTER);
    for (uint k = 0u; k < count; ++k) {
        GpuLight l = frame.lights.items[pc.light_indices.indices[list_base + k]];
        vec3     to_light = l.position - v_world_pos;
        float    d = length(to_light);
        vec3     L = to_light / max(d, 1e-4);
        float    atten = ae_distance_attenuation(d, l.range);
        if (l.type == AE_LIGHT_SPOT) {
            float s = clamp(dot(-L, l.direction) * l.spot_scale + l.spot_offset, 0.0, 1.0);
            atten *= s * s;
            if (atten > 0.0 && l.shadow != AE_INVALID_INDEX) {
                atten *= ae_spot_shadow(frame, l.shadow, v_world_pos, Ng, L, d); // ADR-0012
            }
        } else if (atten > 0.0 && l.shadow != AE_INVALID_INDEX) {
            atten *= ae_point_shadow(frame, l.shadow, v_world_pos, Ng, L, d); // ADR-0015
        }
        if (atten > 0.0) {
            color += ae_evaluate_brdf(N, V, L, diffuse_color, f0, alpha, energy_comp) * l.color * atten;
        }
    }

    // ---- indirect: IBL or flat ambient, occluded by SSAO x material AO ----
    float ssao = 1.0;
#ifndef AE_TRANSLUCENT
    if (pc.ssao_tex != AE_INVALID_INDEX) {
        ssao = textureLod(AE_TEX2D(pc.ssao_tex), gl_FragCoord.xy * frame.inv_viewport, 0.0).r;
    }
#endif
    float ao = min(material_ao, ssao);
    vec3  spec_weight = f0 * dfg.x + dfg.y;
    vec3  env_irradiance; // irradiance / pi of the environment (IBL or flat ambient)
    vec3  indirect_specular;
    if ((frame.flags & AE_FRAME_IBL) != 0u) {
        vec3  R = reflect(-V, N);
        float lod = perceptual_roughness * float(max(frame.prefiltered_mips, 1u) - 1u);
        vec3  prefiltered = textureLod(AE_TEXCUBE(frame.prefiltered_map), R, lod).rgb;
        env_irradiance = texture(AE_TEXCUBE(frame.irradiance_map), N).rgb * frame.ibl_intensity; // pre-divided by pi
        indirect_specular = prefiltered * frame.ibl_intensity;
    } else {
        env_irradiance = frame.ambient;
        indirect_specular = frame.ambient;
    }
    // ADR-0016: inside the GI volume the probes replace the environment's diffuse light, and the
    // environment reflection is dimmed where the probes see less light than the open sky would
    // give (indoors, under overhangs) - a cheap specular occlusion from the same data.
    float gi_w;
    vec3  gi_irradiance = ae_gi_irradiance(frame, v_world_pos, N, Ng, gi_w);
    vec3  irradiance = mix(env_irradiance, gi_irradiance, gi_w);
    if (gi_w > 0.0) {
        float ratio = clamp(ae_luminance(gi_irradiance) / max(ae_luminance(env_irradiance), 1e-4), 0.0, 1.0);
        indirect_specular *= mix(1.0, ratio, gi_w);
    }
    // ADR-0017: reflection probes replace the sky reflection inside their boxes.
    float sky_weight;
    vec3  probe_reflection = ae_probe_reflection(frame, v_world_pos, reflect(-V, N), perceptual_roughness, sky_weight);
    indirect_specular = probe_reflection + indirect_specular * sky_weight;
    vec3 reflection = indirect_specular; // for the debug view
    vec3 indirect_diffuse = diffuse_color * irradiance;
    indirect_diffuse *= (1.0 - spec_weight);
    indirect_specular *= spec_weight * energy_comp;
    color += indirect_diffuse * ao + indirect_specular * ae_specular_occlusion(NoV, ao, alpha);
    color += emissive;

    // ---- debug views ----
    uint dv = frame.debug_view;
    if (dv != AE_DEBUG_NONE) {
        vec3 d = color;
        if (dv == AE_DEBUG_ALBEDO) d = base.rgb;
        else if (dv == AE_DEBUG_NORMALS) d = N * 0.5 + 0.5;
        else if (dv == AE_DEBUG_ROUGHNESS) d = vec3(perceptual_roughness);
        else if (dv == AE_DEBUG_METALLIC) d = vec3(metallic);
        else if (dv == AE_DEBUG_AO) d = vec3(ao);
        else if (dv == AE_DEBUG_EMISSIVE) d = emissive;
        else if (dv == AE_DEBUG_GI) d = irradiance;
        else if (dv == AE_DEBUG_REFLECTIONS) d = reflection;
        else if (dv == AE_DEBUG_LIGHT_COMPLEXITY) d = ae_heatmap(float(count + frame.directional_count) / 32.0);
        else if (dv == AE_DEBUG_SHADOW_CASCADES) {
            d = frame.cascade_count > 0u && view_z < frame.shadow_distance
                    ? cascade_tint(ae_select_cascade(frame, view_z)) * (0.25 + 0.75 * sun_shadow)
                    : vec3(0.5 + 0.5 * sun_shadow);
        }
        out_color = vec4(d, base.a);
        return;
    }

#ifdef AE_TRANSLUCENT
    out_color = vec4(color, base.a);
#else
    out_color = vec4(color, 1.0);
#endif
}
