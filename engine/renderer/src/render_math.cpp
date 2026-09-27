// render_math.cpp — CPU-side projection, culling and cluster math (see render_math.h).
#include "render_math.h"

#include <algorithm>
#include <bit>
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

// ---------------------------------------------------------------------------
// Hi-Z (mirrors shaders/renderer/hiz_build.comp and gpu_cull.comp)
// ---------------------------------------------------------------------------
UVec2 hiz_mip0_size(UVec2 depth_size) {
    return UVec2(std::bit_floor(std::max(depth_size.x, 1u)), std::bit_floor(std::max(depth_size.y, 1u)));
}

u32 hiz_mip_count(UVec2 mip0) { return static_cast<u32>(std::bit_width(std::max(mip0.x, mip0.y))); }

HiZPyramid build_hiz(std::span<const f32> depth, UVec2 depth_size) {
    HiZPyramid  h;
    const UVec2 size0 = hiz_mip0_size(depth_size);
    const u32   mips = hiz_mip_count(size0);
    UVec2       src_size = depth_size;
    const f32*  src = depth.data();
    for (u32 m = 0; m < mips; ++m) {
        const UVec2 dst(std::max(size0.x >> m, 1u), std::max(size0.y >> m, 1u));
        std::vector<f32> out(static_cast<usize>(dst.x) * dst.y, 1.0f);
        for (u32 y = 0; y < dst.y; ++y) {
            for (u32 x = 0; x < dst.x; ++x) {
                const u32 lx = (x * src_size.x) / dst.x;
                const u32 ly = (y * src_size.y) / dst.y;
                const u32 hx = std::max(std::min(((x + 1) * src_size.x + dst.x - 1) / dst.x, src_size.x), lx + 1);
                const u32 hy = std::max(std::min(((y + 1) * src_size.y + dst.y - 1) / dst.y, src_size.y), ly + 1);
                f32 farthest = 1.0f;
                for (u32 sy = ly; sy < hy; ++sy) {
                    for (u32 sx = lx; sx < hx; ++sx) {
                        farthest = std::min(farthest, src[static_cast<usize>(sy) * src_size.x + sx]);
                    }
                }
                out[static_cast<usize>(y) * dst.x + x] = farthest;
            }
        }
        h.sizes.push_back(dst);
        h.levels.push_back(std::move(out));
        src = h.levels.back().data();
        src_size = dst;
    }
    return h;
}

bool hiz_occluded(const HiZPyramid& hiz, UVec2 depth_size, UVec2 px0, UVec2 px1, f32 nearest) {
    if (hiz.levels.empty()) {
        return false;
    }
    const UVec2 size0 = hiz.sizes[0];
    const UVec2 t0((px0.x * size0.x) / depth_size.x, (px0.y * size0.y) / depth_size.y);
    const UVec2 t1((px1.x * size0.x) / depth_size.x, (px1.y * size0.y) / depth_size.y);
    const u32   mips = static_cast<u32>(hiz.levels.size());
    u32         lvl = 0;
    while (lvl + 1 < mips && (((t1.x >> lvl) - (t0.x >> lvl)) > 1u || ((t1.y >> lvl) - (t0.y >> lvl)) > 1u)) {
        ++lvl;
    }
    const UVec2 ls = hiz.sizes[lvl];
    const UVec2 a(std::min(t0.x >> lvl, ls.x - 1), std::min(t0.y >> lvl, ls.y - 1));
    const UVec2 b(std::min(t1.x >> lvl, ls.x - 1), std::min(t1.y >> lvl, ls.y - 1));
    f32 farthest = 1.0f;
    for (u32 y = a.y; y <= b.y; ++y) {
        for (u32 x = a.x; x <= b.x; ++x) {
            farthest = std::min(farthest, hiz.levels[lvl][static_cast<usize>(y) * ls.x + x]);
        }
    }
    return nearest < farthest;
}

bool project_aabb_rect(const Mat4& view_proj, const AABB& box, UVec2 viewport, UVec2& px0, UVec2& px1,
                       f32& nearest) {
    Vec2 mn(3.0e38f), mx(-3.0e38f);
    nearest = 0.0f;
    for (u32 k = 0; k < 8; ++k) {
        const Vec3 c((k & 1u) ? box.max.x : box.min.x, (k & 2u) ? box.max.y : box.min.y,
                     (k & 4u) ? box.max.z : box.min.z);
        const Vec4 clip = view_proj * Vec4(c, 1.0f);
        if (clip.w <= 1e-5f) {
            return false;
        }
        const Vec3 ndc = Vec3(clip) / clip.w;
        mn = glm::min(mn, Vec2(ndc));
        mx = glm::max(mx, Vec2(ndc));
        nearest = std::max(nearest, ndc.z);
    }
    const Vec2 vp(viewport);
    const Vec2 uv0 = glm::clamp(mn * 0.5f + 0.5f, Vec2(0.0f), Vec2(1.0f));
    const Vec2 uv1 = glm::clamp(mx * 0.5f + 0.5f, Vec2(0.0f), Vec2(1.0f));
    px0 = UVec2(glm::min(glm::floor(uv0 * vp), vp - 1.0f));
    px1 = UVec2(glm::min(glm::floor(uv1 * vp), vp - 1.0f));
    return true;
}

} // namespace aether::renderer
