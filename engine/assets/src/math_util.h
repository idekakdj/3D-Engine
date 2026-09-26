// math_util.h — private math helpers for aether.assets.
//
// core/math.h asks engine code not to depend on glm:: outside that header; every glm call
// this module needs is funnelled through this one file so a future custom math backend
// only has to touch it.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <cmath>

namespace aether::assets::detail {

[[nodiscard]] inline f32  dot(const Vec3& a, const Vec3& b) { return glm::dot(a, b); }
[[nodiscard]] inline Vec3 cross(const Vec3& a, const Vec3& b) { return glm::cross(a, b); }
[[nodiscard]] inline f32  length(const Vec3& v) { return glm::length(v); }
[[nodiscard]] inline f32  length(const Vec4& v) { return glm::length(v); }
[[nodiscard]] inline Mat4 inverse(const Mat4& m) { return glm::inverse(m); }

[[nodiscard]] inline bool is_finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Normalises `v`, or returns `fallback` when v is (near) zero or not finite.
[[nodiscard]] inline Vec3 normalize_or(const Vec3& v, const Vec3& fallback) {
    const f32 len = length(v);
    if (!(len > 1e-20f) || !std::isfinite(len)) return fallback;
    return v / len;
}

// Quaternion from glTF component order (x, y, z, w). glm's constructor takes (w, x, y, z).
[[nodiscard]] inline Quat quat_xyzw(f32 x, f32 y, f32 z, f32 w) { return Quat(w, x, y, z); }

[[nodiscard]] inline Quat normalize_quat(const Quat& q) {
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(len > 1e-20f) || !std::isfinite(len)) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    return Quat(q.w / len, q.x / len, q.y / len, q.z / len);
}

// A unit vector perpendicular to unit vector n.
[[nodiscard]] inline Vec3 any_perpendicular(const Vec3& n) {
    const Vec3 axis = std::fabs(n.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    return normalize_or(axis - n * dot(n, axis), Vec3(0.0f, 0.0f, 1.0f));
}

// Column-major float[16] (glTF / glm layout) -> Mat4.
[[nodiscard]] inline Mat4 mat4_from_floats(const f32* m) {
    Mat4 r(1.0f);
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) r[c][row] = m[c * 4 + row];
    }
    return r;
}

[[nodiscard]] inline bool approx_identity(const Mat4& m, f32 eps = 1e-5f) {
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            const f32 expected = c == row ? 1.0f : 0.0f;
            if (std::fabs(m[c][row] - expected) > eps) return false;
        }
    }
    return true;
}

// TRS decomposition of an affine matrix (shear is discarded). A negative determinant is
// folded into scale.x.
[[nodiscard]] inline Transform decompose(const Mat4& m) {
    Transform t;
    t.position = Vec3(m[3]);
    Vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    Vec3 s(length(c0), length(c1), length(c2));
    if (dot(cross(c0, c1), c2) < 0.0f) s.x = -s.x;
    const Vec3 x = std::fabs(s.x) > 1e-20f ? c0 / s.x : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 y = std::fabs(s.y) > 1e-20f ? c1 / s.y : Vec3(0.0f, 1.0f, 0.0f);
    const Vec3 z = std::fabs(s.z) > 1e-20f ? c2 / s.z : Vec3(0.0f, 0.0f, 1.0f);
    t.rotation = normalize_quat(glm::quat_cast(Mat3(x, y, z)));
    t.scale = s;
    return t;
}

} // namespace aether::assets::detail
