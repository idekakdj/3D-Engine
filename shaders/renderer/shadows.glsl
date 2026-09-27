// shaders/renderer/shadows.glsl — cascaded shadow map sampling (reverse-Z, compare sampler).
// The shadow map is a D32F Tex2DArray (one layer per cascade) registered with a
// GreaterEqual compare sampler: 1 = lit (receiver depth >= stored occluder depth).
#ifndef AE_SHADOWS_GLSL
#define AE_SHADOWS_GLSL

// 3x3 bilinear-compare taps (a 4x4 texel tent footprint), Gaussian-like weights.
float ae_shadow_pcf(FrameData frame, uint cascade, vec3 world_pos) {
    vec4 clip = frame.cascade_view_proj[cascade] * vec4(world_pos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    float ref = clamp(ndc.z, 0.0, 1.0); // casters in front of the near plane were clamped to 1
    vec2  texel = 1.0 / vec2(textureSize(AE_SHADOWARRAY(frame.shadow_map), 0).xy);
    float sum = 0.0;
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            vec4  coord = vec4(uv + vec2(x, y) * texel, float(cascade), ref);
            sum += w * textureGrad(AE_SHADOWARRAY(frame.shadow_map), coord, vec2(0.0), vec2(0.0));
            wsum += w;
        }
    }
    return sum / wsum;
}

// Cascade index for a positive view depth (count - 1 beyond the last split).
uint ae_select_cascade(FrameData frame, float view_z) {
    uint c = 0u;
    while (c + 1u < frame.cascade_count && view_z > frame.cascade_splits[c]) {
        ++c;
    }
    return c;
}

// Directional shadow term with normal-offset bias (scaled per cascade by its world texel
// size), cross-cascade blending and distance fade.
float ae_cascaded_shadow(FrameData frame, vec3 world_pos, vec3 geom_normal, vec3 L, float view_z) {
    if (frame.cascade_count == 0u || frame.shadow_map == AE_INVALID_INDEX ||
        view_z >= frame.shadow_distance) {
        return 1.0;
    }
    uint  c = ae_select_cascade(frame, view_z);
    float NoL = clamp(dot(geom_normal, L), 0.0, 1.0);
    float offset = frame.shadow_normal_bias * (1.0 - 0.5 * NoL);
    float s = ae_shadow_pcf(frame, c, world_pos + geom_normal * frame.cascade_texel_world[c] * offset);

    float split_near = c == 0u ? frame.near_z : frame.cascade_splits[c - 1u];
    float split_far = frame.cascade_splits[c];
    float band = max((split_far - split_near) * frame.cascade_blend, 1e-4);
    float t = (split_far - view_z) / band;
    if (t < 1.0 && c + 1u < frame.cascade_count) {
        float s_next = ae_shadow_pcf(frame, c + 1u,
                                     world_pos + geom_normal * frame.cascade_texel_world[c + 1u] * offset);
        s = mix(s_next, s, clamp(t, 0.0, 1.0));
    }
    float fade = clamp((view_z - frame.shadow_fade_start) /
                           max(frame.shadow_distance - frame.shadow_fade_start, 1e-4), 0.0, 1.0);
    return mix(s, 1.0, fade);
}

// ---- spot-light shadows (ADR-0012) ----------------------------------------------------------
// The spot shadow array is sampled WITHOUT a compare sampler: textureGather fetches the 2x2 depth
// quad and the reverse-Z comparison (lit = receiver depth >= occluder depth) and bilinear weights
// are applied here. Same result as hardware PCF, and it avoids the depth-compare path that Mesa
// llvmpipe cannot sample (ADR-0004), so spot shadows also work on software rasterisers.
float ae_spot_pcf_tap(uint map, uint layer, vec2 uv, vec2 size, float ref) {
    vec2  st = uv * size - 0.5;
    vec2  f = fract(st);
    vec2  guv = (floor(st) + 1.0) / size; // centre of the 2x2 quad
    vec4  d = textureGather(AE_TEX2DARRAY(map), vec3(guv, float(layer)), 0);
    // Gather order: x = (0,1), y = (1,1), z = (1,0), w = (0,0).
    vec4  lit = step(d, vec4(ref));
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

// Spot shadow term for GpuSpotShadow `index`: 3x3 bilinear taps (tent weights) with a
// normal-offset bias of ~1.5 shadow texels at the receiver's distance from the light.
float ae_spot_shadow(FrameData frame, uint index, vec3 world_pos, vec3 geom_normal, vec3 L, float dist) {
    if (index >= frame.spot_shadow_count || frame.spot_shadow_map == AE_INVALID_INDEX) {
        return 1.0;
    }
    GpuSpotShadow sh = frame.spot_shadows.items[index];
    float NoL = clamp(dot(geom_normal, L), 0.0, 1.0);
    float texel_world = sh.texel_scale * dist;
    vec3  p = world_pos + geom_normal * texel_world * (1.5 - 0.75 * NoL);
    vec4  clip = sh.view_proj * vec4(p, 1.0);
    if (clip.w <= 1e-5) {
        return 1.0;
    }
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0; // outside the shadow frustum (very wide cones are capped)
    }
    float ref = clamp(ndc.z, 0.0, 1.0);
    vec2  size = vec2(textureSize(AE_TEX2DARRAY(frame.spot_shadow_map), 0).xy);
    vec2  texel = 1.0 / size;
    float sum = 0.0;
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            sum += w * ae_spot_pcf_tap(frame.spot_shadow_map, sh.layer, uv + vec2(x, y) * texel, size, ref);
            wsum += w;
        }
    }
    return sum / wsum;
}

#endif
