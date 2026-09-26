// shadow_math.cpp — cascaded shadow map math (see shadow_math.h).
#include "shadow_math.h"

#include <algorithm>
#include <cmath>

namespace aether::renderer {

std::array<f32, kMaxCascades> compute_cascade_splits(u32 count, f32 near_z, f32 max_distance,
                                                     f32 lambda) {
    std::array<f32, kMaxCascades> out{};
    out.fill(max_distance);
    const u32 n_cascades = std::clamp(count, 1u, kMaxCascades);
    const f32 n = std::max(near_z, 1e-3f);
    const f32 f = std::max(max_distance, n * 1.01f);
    const f32 l = std::clamp(lambda, 0.0f, 1.0f);
    for (u32 i = 1; i <= n_cascades; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n_cascades);
        const f32 log_split = n * std::pow(f / n, t);
        const f32 uni_split = n + (f - n) * t;
        out[i - 1] = l * log_split + (1.0f - l) * uni_split;
    }
    out[n_cascades - 1] = f; // exact, avoids pow round-off
    return out;
}

std::array<Vec3, 8> frustum_slice_corners_view(const Mat4& proj, f32 d0, f32 d1) {
    const Mat4 inv = glm::inverse(proj);
    const bool ortho = std::abs(proj[3][3] - 1.0f) < 1e-6f && std::abs(proj[2][3]) < 1e-6f;
    std::array<Vec3, 8> c{};
    const f32 xs[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const f32 ys[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int i = 0; i < 4; ++i) {
        // Any depth inside the clip volume yields a point on the corner ray.
        Vec4 p = inv * Vec4(xs[i], ys[i], 0.5f, 1.0f);
        p /= p.w;
        const Vec3 v(p);
        if (ortho) {
            c[static_cast<usize>(i)] = Vec3(v.x, v.y, -d0);
            c[static_cast<usize>(i + 4)] = Vec3(v.x, v.y, -d1);
        } else {
            const f32 inv_z = 1.0f / std::max(-v.z, 1e-12f);
            const Vec3 ray = v * inv_z; // point at distance 1 along -Z
            c[static_cast<usize>(i)] = ray * d0;
            c[static_cast<usize>(i + 4)] = ray * d1;
        }
    }
    return c;
}

BoundingSphere cascade_bounding_sphere(const Mat4& proj, const Mat4& inv_view, f32 d0, f32 d1) {
    const std::array<Vec3, 8> corners = frustum_slice_corners_view(proj, d0, d1);
    // Largest lateral slope of the corner rays (tan of the half-diagonal angle).
    f32 k2 = 0.0f;
    for (usize i = 4; i < 8; ++i) {
        const Vec3& p = corners[i];
        const f32 z = std::max(-p.z, 1e-12f);
        k2 = std::max(k2, (p.x * p.x + p.y * p.y) / (z * z));
    }
    // Minimal sphere of a symmetric slice has its center on the view axis at z0.
    f32 z0 = 0.5f * (d0 + d1) * (1.0f + k2);
    z0 = std::min(z0, d1);
    const Vec3 center_view(0.0f, 0.0f, -z0);
    f32 r2 = 0.0f;
    for (const Vec3& p : corners) {
        const Vec3 d = p - center_view;
        r2 = std::max(r2, glm::dot(d, d));
    }
    BoundingSphere s;
    s.radius = std::ceil(std::sqrt(r2) * 16.0f) / 16.0f;
    s.center = Vec3(inv_view * Vec4(center_view, 1.0f));
    return s;
}

Mat4 reverse_z_matrix() {
    Mat4 m(1.0f);
    m[2][2] = -1.0f;
    m[3][2] = 1.0f;
    return m;
}

CascadeMatrices fit_cascade(const BoundingSphere& sphere, const Vec3& light_dir,
                            u32 shadow_map_size, f32 caster_extent) {
    CascadeMatrices out;
    const f32 len = glm::length(light_dir);
    const Vec3 dir = len > 1e-6f ? light_dir / len : Vec3(0.0f, -1.0f, 0.0f);
    const Vec3 up = std::abs(dir.y) > 0.99f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(0.0f, 1.0f, 0.0f);
    const f32 r = std::max(sphere.radius, 1e-3f);

    // Eye at the sphere center looking along the light: the sphere spans view z in [-r, r].
    out.view = glm::lookAtRH(sphere.center, sphere.center + dir, up);
    const f32 near_d = -std::max(r, caster_extent); // plane behind the eye (toward the light)
    const f32 far_d = r;
    Mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, near_d, far_d);
    proj[1][1] *= -1.0f; // same Vulkan Y-flip as core::perspective -> same winding rules
    proj = reverse_z_matrix() * proj;

    // Snap the projected world origin to the texel grid.
    const f32 half = static_cast<f32>(shadow_map_size) * 0.5f;
    const Vec4 origin = proj * out.view * Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const Vec2 texel(origin.x * half, origin.y * half);
    const Vec2 snapped(std::round(texel.x), std::round(texel.y));
    proj[3][0] += (snapped.x - texel.x) / half;
    proj[3][1] += (snapped.y - texel.y) / half;

    out.proj = proj;
    out.view_proj = proj * out.view;
    out.texel_world = (2.0f * r) / static_cast<f32>(std::max(shadow_map_size, 1u));
    return out;
}

} // namespace aether::renderer
