// render_math.cpp — CPU-side projection, culling and cluster math (see render_math.h).
#include "render_math.h"

#include <algorithm>
#include <cmath>

namespace aether::renderer {

// ---------------------------------------------------------------------------
// Projection helpers
// ---------------------------------------------------------------------------
f32 ndc_depth_at(const Mat4& proj, f32 view_distance) {
    const Vec4 clip = proj * Vec4(0.0f, 0.0f, -view_distance, 1.0f);
    return std::abs(clip.w) > 1e-20f ? clip.z / clip.w : clip.z;
}

bool is_reverse_z(const Mat4& proj, f32 near_z) {
    return ndc_depth_at(proj, std::max(near_z, 1e-6f)) > 0.5f;
}

Mat4 to_reverse_z(const Mat4& proj, f32 near_z) {
    if (is_reverse_z(proj, near_z)) {
        return proj;
    }
    Mat4 flip(1.0f);
    flip[2][2] = -1.0f; // z' = -z + w
    flip[3][2] = 1.0f;
    return flip * proj;
}

Mat4 apply_clip_jitter(const Mat4& proj, Vec2 jitter_ndc) {
    // Left-multiply in clip space: j * (proj * p). Column 3 of j is scaled by clip.w, so
    // clip.xy' = clip.xy + jitter * clip.w, i.e. an NDC offset of exactly `jitter_ndc`.
    Mat4 j(1.0f);
    j[3][0] = jitter_ndc.x;
    j[3][1] = jitter_ndc.y;
    return j * proj;
}

f32 halton(u32 index, u32 base) {
    f32 f = 1.0f;
    f32 r = 0.0f;
    u32 i = index;
    const f32 inv_base = 1.0f / static_cast<f32>(base);
    while (i > 0) {
        f *= inv_base;
        r += f * static_cast<f32>(i % base);
        i /= base;
    }
    return r;
}

Vec2 taa_jitter_pixels(u64 frame, u32 sample_count) {
    const u32 n = sample_count == 0 ? 1u : sample_count;
    const u32 idx = static_cast<u32>(frame % n) + 1u; // skip index 0 (= 0,0)
    return Vec2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
}

// ---------------------------------------------------------------------------
// Culling
// ---------------------------------------------------------------------------
namespace {
Vec4 normalize_plane(const Vec4& p) {
    const f32 len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return len > 1e-20f ? p / len : p;
}
Vec4 row(const Mat4& m, int r) { return Vec4(m[0][r], m[1][r], m[2][r], m[3][r]); }
} // namespace

Frustum extract_frustum(const Mat4& vp, bool reverse_z, bool skip_near_plane) {
    const Vec4 r0 = row(vp, 0);
    const Vec4 r1 = row(vp, 1);
    const Vec4 r2 = row(vp, 2);
    const Vec4 r3 = row(vp, 3);
    Frustum f;
    f.planes[0] = normalize_plane(r3 + r0); // left   : x >= -w
    f.planes[1] = normalize_plane(r3 - r0); // right  : x <=  w
    f.planes[2] = normalize_plane(r3 + r1); // bottom : y >= -w
    f.planes[3] = normalize_plane(r3 - r1); // top    : y <=  w
    // Vulkan depth: 0 <= z <= w. With reverse-Z the "z <= w" plane is the near plane.
    const Vec4 z_ge_0 = normalize_plane(r2);      // z >= 0
    const Vec4 z_le_w = normalize_plane(r3 - r2); // z <= w
    const Vec4 near_plane = reverse_z ? z_le_w : z_ge_0;
    const Vec4 far_plane = reverse_z ? z_ge_0 : z_le_w;
    if (skip_near_plane) {
        f.planes[4] = far_plane;
        f.plane_count = 5;
    } else {
        f.planes[4] = near_plane;
        f.planes[5] = far_plane;
        f.plane_count = 6;
    }
    return f;
}

bool frustum_intersects_aabb(const Frustum& f, const AABB& box) {
    const Vec3 c = box.center();
    const Vec3 e = box.extent();
    for (u32 i = 0; i < f.plane_count; ++i) {
        const Vec4& p = f.planes[i];
        const f32 d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        const f32 r = std::abs(p.x) * e.x + std::abs(p.y) * e.y + std::abs(p.z) * e.z;
        if (d + r < 0.0f) {
            return false;
        }
    }
    return true;
}

bool frustum_intersects_sphere(const Frustum& f, const Vec3& c, f32 radius) {
    for (u32 i = 0; i < f.plane_count; ++i) {
        const Vec4& p = f.planes[i];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -radius) {
            return false;
        }
    }
    return true;
}

AABB transform_aabb(const AABB& local, const Mat4& m) {
    AABB out;
    const Vec3 t(m[3]);
    out.min = t;
    out.max = t;
    for (int col = 0; col < 3; ++col) {
        for (int r = 0; r < 3; ++r) {
            const f32 a = m[col][r] * local.min[col];
            const f32 b = m[col][r] * local.max[col];
            out.min[r] += std::min(a, b);
            out.max[r] += std::max(a, b);
        }
    }
    return out;
}

bool aabb_is_unset(const AABB& b) {
    return b.min == b.max && b.min == Vec3(0.0f);
}

// ---------------------------------------------------------------------------
// Clusters
// ---------------------------------------------------------------------------
ClusterParams make_cluster_params(UVec2 viewport, f32 near_z, f32 far_z) {
    ClusterParams p;
    const f32 n = std::max(near_z, 1e-4f);
    const f32 f = std::max(far_z, n * 1.001f);
    const f32 log_ratio = std::log(f / n);
    p.z_scale = static_cast<f32>(kClusterZ) / log_ratio;
    p.z_bias = static_cast<f32>(kClusterZ) * std::log(n) / log_ratio;
    p.tile_w = std::max(1u, (viewport.x + kClusterX - 1) / kClusterX);
    p.tile_h = std::max(1u, (viewport.y + kClusterY - 1) / kClusterY);
    return p;
}

u32 cluster_slice(f32 view_z, const ClusterParams& p) {
    const f32 s = std::floor(std::log(std::max(view_z, 1e-6f)) * p.z_scale - p.z_bias);
    return static_cast<u32>(std::clamp(s, 0.0f, static_cast<f32>(kClusterZ - 1)));
}

f32 cluster_slice_near(u32 s, f32 near_z, f32 far_z) {
    return near_z * std::pow(far_z / near_z, static_cast<f32>(s) / static_cast<f32>(kClusterZ));
}

u32 cluster_index_for(Vec2 frag, f32 view_z, const ClusterParams& p) {
    const u32 x = std::min(static_cast<u32>(std::max(frag.x, 0.0f)) / p.tile_w, kClusterX - 1);
    const u32 y = std::min(static_cast<u32>(std::max(frag.y, 0.0f)) / p.tile_h, kClusterY - 1);
    return cluster_index(x, y, cluster_slice(view_z, p));
}

Sphere spot_bounding_sphere(const Vec3& apex, const Vec3& dir, f32 range, f32 cos_outer) {
    // Wronski, "Cull that cone": minimal sphere around a cone of height `range`.
    const f32 c = std::clamp(cos_outer, 1e-4f, 1.0f);
    Sphere s;
    if (c < 0.70710678f) { // half-angle > 45 degrees
        const f32 sin_a = std::sqrt(std::max(0.0f, 1.0f - c * c));
        s.center = apex + dir * (range * c);
        s.radius = range * sin_a;
    } else {
        s.radius = range / (2.0f * c);
        s.center = apex + dir * s.radius;
    }
    return s;
}

f32 distance_attenuation(f32 distance, f32 range) {
    const f32 d2 = distance * distance;
    const f32 r = std::max(range, 1e-4f);
    const f32 ratio = d2 / (r * r);
    const f32 window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
    return (window * window) / std::max(d2, 1e-4f);
}

} // namespace aether::renderer
