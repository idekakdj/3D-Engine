// shaders/renderer/gi.glsl — irradiance probe volume sampling (ADR-0016).
// Probes sit on a regular grid (frame.gi_min + i / gi_inv_spacing). A surface blends its 8
// surrounding probes trilinearly, weighted DDGI-style by how much each probe lies in front of the
// surface (smooth back-face weight) and down-weighting probes that saw mostly back faces (they sit
// inside geometry). The sample point is pushed along the geometric normal to limit leaking.
#ifndef AE_GI_GLSL
#define AE_GI_GLSL

vec3 ae_gi_probe_irradiance(GpuGiProbe p, vec3 n) {
    return vec3(p.r.x + dot(p.r.yzw, n), p.g.x + dot(p.g.yzw, n), p.b.x + dot(p.b.yzw, n));
}

// Irradiance / pi at `pos` for shading normal `N` (geometric normal `Ng` for the offset).
// `weight` = 1 well inside the volume, fading to 0 over one probe cell outside it (and 0 while
// no surrounding probe has been captured yet): mix with the environment by it.
vec3 ae_gi_irradiance(FrameData frame, vec3 pos, vec3 N, vec3 Ng, out float weight) {
    weight = 0.0;
    if (frame.gi_counts.x < 2u) {
        return vec3(0.0); // no volume (gi_counts is zeroed when GI is off; no 64-bit compare)
    }
    uvec3 counts = frame.gi_counts;
    vec3  maxg = vec3(counts - 1u);
    vec3  g = (pos + Ng * frame.gi_normal_bias - frame.gi_min) * frame.gi_inv_spacing;
    vec3  outside = max(max(-g, g - maxg), vec3(0.0));
    float fade = clamp(1.0 - max(outside.x, max(outside.y, outside.z)), 0.0, 1.0);
    if (fade <= 0.0) {
        return vec3(0.0);
    }
    vec3  gc = clamp(g, vec3(0.0), maxg);
    uvec3 base = min(uvec3(gc), counts - 2u);
    vec3  f = gc - vec3(base);
    vec3  spacing = 1.0 / frame.gi_inv_spacing;
    vec3  sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < 8u; ++i) {
        uvec3 o = uvec3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
        uvec3 c = base + o;
        GpuGiProbe p = frame.gi_probes.items[c.x + counts.x * (c.y + counts.y * c.z)];
        if (p.meta.y <= 0.0) {
            continue; // not captured yet
        }
        vec3  tri = mix(1.0 - f, f, vec3(o));
        float w = tri.x * tri.y * tri.z;
        vec3  to_probe = frame.gi_min + vec3(c) * spacing - pos;
        float wrap = (dot(ae_safe_normalize(to_probe), Ng) + 1.0) * 0.5;
        w *= wrap * wrap + 0.2;
        if (p.meta.x > 0.2) {
            w *= 0.02; // inside geometry: only used when nothing better is around
        }
        w = max(w, 1e-6);
        sum += ae_gi_probe_irradiance(p, N) * w;
        wsum += w;
    }
    if (wsum <= 0.0) {
        return vec3(0.0);
    }
    weight = fade;
    return max(sum / wsum, vec3(0.0)) * frame.gi_intensity;
}

#endif
