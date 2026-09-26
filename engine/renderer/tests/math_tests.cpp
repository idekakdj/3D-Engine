// math_tests.cpp — CPU renderer math: Halton/jitter, reverse-Z, frustum/AABB culling,
// cascade splits + stabilized fitting + texel snapping, cluster indexing, light bounds.
#include "format_utils.h"
#include "render_math.h"
#include "shadow_math.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace aether;
using namespace aether::renderer;

namespace {
Mat4 camera_proj() { return to_reverse_z(perspective(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 500.0f), 0.1f); }

f32 ndc_z(const Mat4& vp, const Vec3& p) {
    const Vec4 c = vp * Vec4(p, 1.0f);
    return c.z / c.w;
}
} // namespace

TEST_CASE("halton sequence and TAA jitter") {
    CHECK(halton(1, 2) == doctest::Approx(0.5f));
    CHECK(halton(2, 2) == doctest::Approx(0.25f));
    CHECK(halton(3, 2) == doctest::Approx(0.75f));
    CHECK(halton(1, 3) == doctest::Approx(1.0f / 3.0f));
    CHECK(halton(2, 3) == doctest::Approx(2.0f / 3.0f));
    CHECK(halton(4, 3) == doctest::Approx(1.0f / 3.0f + 1.0f / 9.0f));
    Vec2 sum(0.0f);
    for (u64 f = 0; f < 8; ++f) {
        const Vec2 j = taa_jitter_pixels(f);
        CHECK(j.x >= -0.5f);
        CHECK(j.x < 0.5f);
        CHECK(j.y >= -0.5f);
        CHECK(j.y < 0.5f);
        CHECK(taa_jitter_pixels(f + 8) == j); // 8-sample cycle
        sum += j;
    }
    CHECK(std::abs(sum.x / 8.0f) < 0.1f); // roughly centred
    CHECK(std::abs(sum.y / 8.0f) < 0.1f);
}

TEST_CASE("reverse-Z conversion and clip-space jitter") {
    const Mat4 std_proj = perspective(60.0f * kDeg2Rad, 1.5f, 0.1f, 100.0f);
    CHECK_FALSE(is_reverse_z(std_proj, 0.1f));
    const Mat4 rz = to_reverse_z(std_proj, 0.1f);
    CHECK(is_reverse_z(rz, 0.1f));
    CHECK(ndc_depth_at(rz, 0.1f) == doctest::Approx(1.0f));
    CHECK(ndc_depth_at(rz, 100.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(to_reverse_z(rz, 0.1f) == rz); // idempotent

    const Mat4 j = apply_clip_jitter(rz, Vec2(0.01f, -0.02f));
    const Vec4 a = rz * Vec4(1.0f, 2.0f, -5.0f, 1.0f);
    const Vec4 b = j * Vec4(1.0f, 2.0f, -5.0f, 1.0f);
    CHECK(b.x / b.w - a.x / a.w == doctest::Approx(0.01f));
    CHECK(b.y / b.w - a.y / a.w == doctest::Approx(-0.02f));
    CHECK(b.z / b.w == doctest::Approx(a.z / a.w));
}

TEST_CASE("frustum culling of AABBs") {
    const Mat4    view = look_at(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Frustum fr = extract_frustum(camera_proj() * view, true, false);
    auto box = [](Vec3 c, f32 e) { return AABB{ c - Vec3(e), c + Vec3(e) }; };
    CHECK(frustum_intersects_aabb(fr, box(Vec3(0, 0, -10), 1.0f)));
    CHECK_FALSE(frustum_intersects_aabb(fr, box(Vec3(0, 0, 10), 1.0f)));     // behind
    CHECK_FALSE(frustum_intersects_aabb(fr, box(Vec3(0, 0, -600), 1.0f)));   // beyond far
    CHECK_FALSE(frustum_intersects_aabb(fr, box(Vec3(100, 0, -10), 1.0f)));  // far right
    CHECK(frustum_intersects_aabb(fr, box(Vec3(0, 0, -0.5f), 1.0f)));        // straddles near
    CHECK(frustum_intersects_aabb(fr, box(Vec3(11, 0, -10), 3.0f)));         // straddles right plane (x~10.3)
    CHECK(frustum_intersects_sphere(fr, Vec3(0, 0, -50), 1.0f));
    CHECK_FALSE(frustum_intersects_sphere(fr, Vec3(0, 0, 50), 1.0f));

    // Skipping the near plane keeps boxes behind the eye (shadow caster pancaking).
    const Frustum no_near = extract_frustum(camera_proj() * view, true, true);
    CHECK(no_near.plane_count == 5);

    // Arvo transform: rotated unit box grows to sqrt(2) half-extent in x/z.
    const Mat4 rot = glm::rotate(Mat4(1.0f), kPi / 4.0f, Vec3(0, 1, 0));
    const AABB t = transform_aabb(AABB{ Vec3(-1), Vec3(1) }, glm::translate(Mat4(1.0f), Vec3(5, 0, 0)) * rot);
    CHECK(t.min.x == doctest::Approx(5.0f - std::sqrt(2.0f)));
    CHECK(t.max.z == doctest::Approx(std::sqrt(2.0f)));
    CHECK(t.max.y == doctest::Approx(1.0f));
    CHECK(aabb_is_unset(AABB{}));
    CHECK_FALSE(aabb_is_unset(t));
}

TEST_CASE("cascade split distribution") {
    const auto s = compute_cascade_splits(4, 0.1f, 100.0f, 0.75f);
    CHECK(s[3] == doctest::Approx(100.0f));
    for (int i = 1; i < 4; ++i) {
        CHECK(s[i] > s[i - 1]);
    }
    CHECK(s[0] > 0.1f);
    const auto uni = compute_cascade_splits(4, 0.1f, 100.0f, 0.0f);
    CHECK(uni[0] == doctest::Approx(0.1f + 99.9f * 0.25f));
    const auto lg = compute_cascade_splits(4, 0.1f, 100.0f, 1.0f);
    CHECK(lg[1] == doctest::Approx(0.1f * std::pow(1000.0f, 0.5f)));
    CHECK(compute_cascade_splits(1, 0.1f, 50.0f)[0] == doctest::Approx(50.0f));
}

TEST_CASE("cascade bounding sphere is rotation invariant and encloses the slice") {
    const Mat4 proj = camera_proj();
    const Mat4 v0 = look_at(Vec3(3, 2, 1), Vec3(3, 2, -10), Vec3(0, 1, 0));
    const Mat4 v1 = look_at(Vec3(3, 2, 1), Vec3(10, -4, 7), Vec3(0, 1, 0));
    const BoundingSphere a = cascade_bounding_sphere(proj, glm::inverse(v0), 5.0f, 20.0f);
    const BoundingSphere b = cascade_bounding_sphere(proj, glm::inverse(v1), 5.0f, 20.0f);
    CHECK(a.radius == doctest::Approx(b.radius));
    for (const Vec3& c : frustum_slice_corners_view(proj, 5.0f, 20.0f)) {
        const Vec3 w = Vec3(glm::inverse(v0) * Vec4(c, 1.0f));
        CHECK(glm::length(w - a.center) <= a.radius + 1e-3f);
    }
}

TEST_CASE("cascade fit: reverse-Z ortho with texel-snapped origin") {
    const u32            size = 1024;
    const Vec3           light = glm::normalize(Vec3(0.3f, -1.0f, 0.2f));
    const BoundingSphere s0{ Vec3(10.0f, 0.0f, -4.0f), 12.5f };
    const CascadeMatrices m0 = fit_cascade(s0, light, size, 0.0f);

    // World origin lands exactly on a texel boundary.
    const Vec4 o = m0.view_proj * Vec4(0, 0, 0, 1);
    const f32  tx = o.x * size * 0.5f;
    const f32  ty = o.y * size * 0.5f;
    CHECK(std::abs(tx - std::round(tx)) < 1e-2f);
    CHECK(std::abs(ty - std::round(ty)) < 1e-2f);
    CHECK(m0.texel_world == doctest::Approx(25.0f / 1024.0f));

    // Moving the sphere by a sub-texel amount moves static geometry by whole texels only.
    BoundingSphere s1 = s0;
    s1.center += Vec3(0.0037f, 0.0f, 0.0021f);
    const CascadeMatrices m1 = fit_cascade(s1, light, size, 0.0f);
    const Vec3 p(4.0f, 1.0f, -7.0f);
    const Vec4 a = m0.view_proj * Vec4(p, 1.0f);
    const Vec4 b = m1.view_proj * Vec4(p, 1.0f);
    const f32  dx = (b.x - a.x) * size * 0.5f;
    const f32  dy = (b.y - a.y) * size * 0.5f;
    CHECK(std::abs(dx - std::round(dx)) < 1e-2f);
    CHECK(std::abs(dy - std::round(dy)) < 1e-2f);

    // Reverse-Z: the sphere point nearest the light maps to larger depth than the farthest.
    const f32 z_near = ndc_z(m0.view_proj, s0.center - light * (s0.radius * 0.9f));
    const f32 z_far = ndc_z(m0.view_proj, s0.center + light * (s0.radius * 0.9f));
    CHECK(z_near > z_far);
    CHECK(z_near <= 1.0f);
    CHECK(z_far >= 0.0f);

    // Caster extension keeps an occluder far toward the light inside [0, 1].
    const CascadeMatrices ext = fit_cascade(s0, light, size, 60.0f);
    const f32 z_caster = ndc_z(ext.view_proj, s0.center - light * 55.0f);
    CHECK(z_caster <= 1.0f);
    CHECK(z_caster >= 0.0f);
}

TEST_CASE("cluster index math") {
    const ClusterParams p = make_cluster_params(UVec2(1920, 1080), 0.1f, 1000.0f);
    CHECK(p.tile_w == 120);
    CHECK(p.tile_h == 120);
    CHECK(cluster_slice(0.1f, p) == 0);
    CHECK(cluster_slice(0.05f, p) == 0);          // clamped
    CHECK(cluster_slice(999.0f, p) == kClusterZ - 1);
    CHECK(cluster_slice(5000.0f, p) == kClusterZ - 1); // clamped
    for (u32 s = 0; s < kClusterZ; ++s) {
        const f32 z0 = cluster_slice_near(s, 0.1f, 1000.0f);
        const f32 z1 = cluster_slice_near(s + 1, 0.1f, 1000.0f);
        CHECK(z1 > z0);
        CHECK(cluster_slice(std::sqrt(z0 * z1), p) == s); // geometric midpoint lies inside
    }
    CHECK(cluster_slice_near(kClusterZ, 0.1f, 1000.0f) == doctest::Approx(1000.0f));
    CHECK(cluster_index(0, 0, 0) == 0);
    CHECK(cluster_index(kClusterX - 1, kClusterY - 1, kClusterZ - 1) == kClusterCount - 1);
    CHECK(cluster_index(1, 0, 0) == 1);
    CHECK(cluster_index(0, 1, 0) == kClusterX);
    CHECK(cluster_index(0, 0, 1) == kClusterX * kClusterY);
    CHECK(cluster_index_for(Vec2(1919.5f, 1079.5f), 999.0f, p) == kClusterCount - 1);
    CHECK(cluster_index_for(Vec2(130.0f, 5.0f), 0.1f, p) == 1);
}

TEST_CASE("light bounds and attenuation") {
    const Vec3   apex(1.0f, 2.0f, 3.0f);
    const Vec3   dir(0.0f, -1.0f, 0.0f);
    for (f32 cos_outer : { 0.95f, 0.8f, 0.5f, 0.1f }) {
        const Sphere s = spot_bounding_sphere(apex, dir, 10.0f, cos_outer);
        const f32    sin_o = std::sqrt(1.0f - cos_outer * cos_outer);
        const Vec3   rim = apex + dir * (10.0f * cos_outer) + Vec3(1, 0, 0) * (10.0f * sin_o);
        CHECK(glm::length(apex - s.center) <= s.radius + 1e-3f);
        CHECK(glm::length(rim - s.center) <= s.radius + 1e-3f);
        CHECK(glm::length(apex + dir * 10.0f - s.center) <= s.radius + 1e-3f);
    }
    CHECK(distance_attenuation(10.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(distance_attenuation(12.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(distance_attenuation(1.0f, 10.0f) > distance_attenuation(2.0f, 10.0f));
    CHECK(distance_attenuation(1.0f, 1000.0f) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("float -> half conversion") {
    CHECK(f32_to_f16(0.0f) == 0x0000);
    CHECK(f32_to_f16(-0.0f) == 0x8000);
    CHECK(f32_to_f16(1.0f) == 0x3C00);
    CHECK(f32_to_f16(-2.0f) == 0xC000);
    CHECK(f32_to_f16(65504.0f) == 0x7BFF);
    CHECK(f32_to_f16(1e6f) == 0x7C00);            // overflow -> inf
    CHECK(f32_to_f16(5.9604645e-8f) == 0x0001);   // smallest subnormal
    CHECK((f32_to_f16(std::nanf("")) & 0x7C00) == 0x7C00);
    CHECK(full_mip_count(1024, 512) == 11);
    CHECK(full_mip_count(1, 1) == 1);
}
