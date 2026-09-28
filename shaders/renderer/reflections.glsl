// shaders/renderer/reflections.glsl — reflection capture probes (ADR-0017).
// Probes are sorted smallest box first. Each probe covering the point contributes its prefiltered
// cube, looked up along the reflection ray projected onto its box (parallax correction), weighted by
// the distance to the box faces (blend_distance); the remaining weight goes to the next probe and
// finally to the sky.
#ifndef AE_REFLECTIONS_GLSL
#define AE_REFLECTIONS_GLSL

// Weighted probe reflection for position `pos`, reflection direction `R` and perceptual roughness.
// Returns the probes' radiance (already weighted); `remaining` = weight left for the sky (1 = no probe).
vec3 ae_probe_reflection(FrameData frame, vec3 pos, vec3 R, float roughness, out float remaining) {
    remaining = 1.0;
    vec3 sum = vec3(0.0);
    // Explicit early-out: Mesa llvmpipe hoists the first probe load above the loop condition and
    // would dereference the (unused) buffer address even with a count of 0.
    if (frame.reflection_probe_count == 0u) {
        return sum;
    }
    for (uint i = 0u; i < frame.reflection_probe_count && remaining > 0.001; ++i) {
        GpuReflectionProbe p = frame.reflection_probes.items[i];
        vec3 d = min(pos - p.box_min, p.box_max - pos);
        float inside = min(d.x, min(d.y, d.z));
        if (inside <= 0.0) {
            continue;
        }
        float w = clamp(inside / max(p.blend_distance, 1e-3), 0.0, 1.0);
        // Box projection: where the ray leaves the box, seen from the capture point.
        vec3  rs = vec3(R.x >= 0.0 ? max(R.x, 1e-5) : min(R.x, -1e-5), R.y >= 0.0 ? max(R.y, 1e-5) : min(R.y, -1e-5),
                        R.z >= 0.0 ? max(R.z, 1e-5) : min(R.z, -1e-5));
        vec3  t_far = max((p.box_max - pos) / rs, (p.box_min - pos) / rs);
        float t = min(t_far.x, min(t_far.y, t_far.z));
        vec3  dir = pos + R * max(t, 0.0) - p.position;
        float lod = roughness * float(max(p.mips, 1u) - 1u);
        vec3  c = textureLod(AE_TEXCUBE(p.cube), dir, lod).rgb * p.intensity;
        sum += c * (w * remaining);
        remaining *= 1.0 - w;
    }
    return sum;
}

#endif
