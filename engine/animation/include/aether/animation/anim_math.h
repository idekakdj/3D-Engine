// aether/animation/anim_math.h — quaternion / TRS helpers for the animation runtime.
//
// Conventions (identical to core/math.h and glTF):
//   * right-handed, +Y up, column vectors, column-major matrices;
//   * quaternions rotate vectors as v' = q * v, and q1 * q2 applies q2 first;
//   * compose(parent, child) == parent * child (child applied first) - the TRS analogue of
//     the matrix product P * C. TRS composition is exact for uniform scale; non-uniform scale
//     is propagated component-wise (no shear), the usual game-engine approximation.
//
// This header is the animation module's single point of contact with the math backend's free
// functions (core/math.h asks modules not to depend on glm:: directly), so a future core SIMD
// library only needs these wrappers ported.
//
// Thread-safety: every function is pure (thread-safe, allocation-free).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <algorithm>
#include <cmath>

namespace aether::animation {

inline constexpr f32 kAnimEpsilon = 1.0e-6f;

// ---- vectors ------------------------------------------------------------------------------------
[[nodiscard]] inline f32 dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] inline Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
[[nodiscard]] inline f32 length_squared(const Vec3& v) noexcept { return dot(v, v); }
[[nodiscard]] inline f32 length(const Vec3& v) noexcept { return std::sqrt(dot(v, v)); }
// Unit vector along `v`, or `fallback` when `v` is (near) zero.
[[nodiscard]] inline Vec3 normalize_or(const Vec3& v, const Vec3& fallback) noexcept {
    const f32 len2 = dot(v, v);
    return len2 > 1.0e-12f ? v * (1.0f / std::sqrt(len2)) : fallback;
}
// Component-wise reciprocal; zero components map to zero instead of infinity.
[[nodiscard]] inline Vec3 safe_reciprocal(const Vec3& v) noexcept {
    return Vec3(std::abs(v.x) > kAnimEpsilon ? 1.0f / v.x : 0.0f,
                std::abs(v.y) > kAnimEpsilon ? 1.0f / v.y : 0.0f,
                std::abs(v.z) > kAnimEpsilon ? 1.0f / v.z : 0.0f);
}

// ---- quaternions --------------------------------------------------------------------------------
// Component-named construction: independent of the backend's constructor argument order.
[[nodiscard]] inline Quat make_quat(f32 w, f32 x, f32 y, f32 z) noexcept {
    Quat q;
    q.w = w;
    q.x = x;
    q.y = y;
    q.z = z;
    return q;
}
[[nodiscard]] inline Quat quat_identity() noexcept { return make_quat(1.0f, 0.0f, 0.0f, 0.0f); }
// glTF / asset_types storage order is (x, y, z, w).
[[nodiscard]] inline Quat quat_from_xyzw(const Vec4& v) noexcept {
    return make_quat(v.w, v.x, v.y, v.z);
}
[[nodiscard]] inline Vec4 quat_to_xyzw(const Quat& q) noexcept { return Vec4(q.x, q.y, q.z, q.w); }

[[nodiscard]] inline f32 dot(const Quat& a, const Quat& b) noexcept {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] inline Quat conjugate(const Quat& q) noexcept { return make_quat(q.w, -q.x, -q.y, -q.z); }
// Unit quaternion, or identity for a degenerate (zero / NaN) input.
[[nodiscard]] inline Quat normalize(const Quat& q) noexcept {
    const f32 len2 = dot(q, q);
    if (!(len2 > 1.0e-12f)) {
        return quat_identity();
    }
    const f32 inv = 1.0f / std::sqrt(len2);
    return make_quat(q.w * inv, q.x * inv, q.y * inv, q.z * inv);
}
[[nodiscard]] inline Quat angle_axis(f32 angle, const Vec3& unit_axis) noexcept {
    const f32 s = std::sin(0.5f * angle);
    return make_quat(std::cos(0.5f * angle), unit_axis.x * s, unit_axis.y * s, unit_axis.z * s);
}
// Rotation angle of a unit quaternion, in [0, pi] (sign-agnostic).
[[nodiscard]] inline f32 quat_angle(const Quat& q) noexcept {
    return 2.0f * std::acos(std::min(std::abs(q.w), 1.0f));
}
// Wraps an angle to [-pi, pi).
[[nodiscard]] inline f32 wrap_angle(f32 a) noexcept {
    return a - kTwoPi * std::floor((a + kPi) / kTwoPi);
}

// Normalized lerp along the shortest arc (hemisphere-corrected). Exact at t = 0 and t = 1.
[[nodiscard]] inline Quat nlerp(const Quat& a, const Quat& b, f32 t) noexcept {
    const f32 s = dot(a, b) < 0.0f ? -1.0f : 1.0f;
    return normalize(make_quat(a.w + (s * b.w - a.w) * t, a.x + (s * b.x - a.x) * t,
                               a.y + (s * b.y - a.y) * t, a.z + (s * b.z - a.z) * t));
}

// Constant-angular-velocity interpolation along the shortest arc.
[[nodiscard]] inline Quat slerp(const Quat& a, const Quat& b, f32 t) noexcept {
    f32  cos_theta = dot(a, b);
    Quat end = b;
    if (cos_theta < 0.0f) {
        end = make_quat(-b.w, -b.x, -b.y, -b.z);
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) {
        return nlerp(a, end, t); // nearly parallel: nlerp is accurate and avoids 0/0
    }
    const f32 theta = std::acos(cos_theta);
    const f32 inv_sin = 1.0f / std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) * inv_sin;
    const f32 wb = std::sin(t * theta) * inv_sin;
    return normalize(make_quat(a.w * wa + end.w * wb, a.x * wa + end.x * wb,
                               a.y * wa + end.y * wb, a.z * wa + end.z * wb));
}

// Shortest-arc rotation taking unit vector `from` onto unit vector `to`.
[[nodiscard]] inline Quat rotation_between(const Vec3& from, const Vec3& to) noexcept {
    const f32 d = dot(from, to);
    if (d >= 1.0f - 1.0e-6f) {
        return quat_identity();
    }
    if (d <= -1.0f + 1.0e-6f) { // opposite: rotate 180 degrees about any perpendicular axis
        Vec3 axis = cross(Vec3(1.0f, 0.0f, 0.0f), from);
        if (length_squared(axis) < 1.0e-6f) {
            axis = cross(Vec3(0.0f, 1.0f, 0.0f), from);
        }
        axis = normalize_or(axis, Vec3(0.0f, 0.0f, 1.0f));
        return make_quat(0.0f, axis.x, axis.y, axis.z);
    }
    const Vec3 c = cross(from, to);
    return normalize(make_quat(1.0f + d, c.x, c.y, c.z));
}

// Twist component of `q` about `unit_axis` (swing-twist decomposition: q = swing * twist =
// twist * swing', with swing/swing' having no rotation about the axis).
[[nodiscard]] inline Quat twist(const Quat& q, const Vec3& unit_axis) noexcept {
    const f32 p = q.x * unit_axis.x + q.y * unit_axis.y + q.z * unit_axis.z;
    const Quat t = make_quat(q.w, unit_axis.x * p, unit_axis.y * p, unit_axis.z * p);
    return dot(t, t) > 1.0e-12f ? normalize(t) : quat_identity();
}
// Signed twist angle of `q` about `unit_axis`, in [-pi, pi).
[[nodiscard]] inline f32 twist_angle(const Quat& q, const Vec3& unit_axis) noexcept {
    const f32 p = q.x * unit_axis.x + q.y * unit_axis.y + q.z * unit_axis.z;
    if (std::abs(p) < 1.0e-9f && std::abs(q.w) < 1.0e-9f) {
        return 0.0f; // pure 180-degree swing: twist undefined
    }
    return wrap_angle(2.0f * std::atan2(p, q.w));
}
// Rotation of `angle` radians about +Y.
[[nodiscard]] inline Quat yaw_quat(f32 angle) noexcept {
    return make_quat(std::cos(0.5f * angle), 0.0f, std::sin(0.5f * angle), 0.0f);
}
// Rotates `v` about +Y by `angle` radians (right-handed: +Z goes to +X at +90 degrees).
[[nodiscard]] inline Vec3 rotate_yaw(f32 angle, const Vec3& v) noexcept {
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    return Vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// ---- matrices -----------------------------------------------------------------------------------
[[nodiscard]] inline Mat4 inverse(const Mat4& m) noexcept { return glm::inverse(m); }
[[nodiscard]] inline Vec3 transform_point(const Mat4& m, const Vec3& p) noexcept {
    return Vec3(m * Vec4(p, 1.0f));
}
// Rotation part of an affine matrix (columns renormalized to strip scale).
[[nodiscard]] inline Quat rotation_from_matrix(const Mat4& m) noexcept {
    const Vec3 c0 = normalize_or(Vec3(m[0]), Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 c1 = normalize_or(Vec3(m[1]), Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 c2 = normalize_or(Vec3(m[2]), Vec3(0.0f, 0.0f, 1.0f));
    return normalize(glm::quat_cast(Mat3(c0, c1, c2)));
}

// ---- TRS transforms -----------------------------------------------------------------------------
[[nodiscard]] inline Transform make_transform(const Vec3& position, const Quat& rotation,
                                              const Vec3& scale = Vec3(1.0f)) noexcept {
    Transform t;
    t.position = position;
    t.rotation = rotation;
    t.scale = scale;
    return t;
}
// Translation/scale lerp + shortest-arc nlerp rotation. Exact at t = 0 and t = 1.
[[nodiscard]] inline Transform lerp(const Transform& a, const Transform& b, f32 t) noexcept {
    return make_transform(a.position + (b.position - a.position) * t,
                          nlerp(a.rotation, b.rotation, t), a.scale + (b.scale - a.scale) * t);
}
// parent * child (child applied first).
[[nodiscard]] inline Transform compose(const Transform& parent, const Transform& child) noexcept {
    return make_transform(parent.position + parent.rotation * (parent.scale * child.position),
                          parent.rotation * child.rotation, parent.scale * child.scale);
}
[[nodiscard]] inline Transform inverse(const Transform& t) noexcept {
    const Vec3 inv_scale = safe_reciprocal(t.scale);
    const Quat inv_rot = conjugate(t.rotation);
    return make_transform(inv_scale * (inv_rot * -t.position), inv_rot, inv_scale);
}
[[nodiscard]] inline Vec3 transform_point(const Transform& t, const Vec3& p) noexcept {
    return t.position + t.rotation * (t.scale * p);
}
// T * R * S as a matrix (same result as Transform::to_matrix, without the extra products).
[[nodiscard]] inline Mat4 to_mat4(const Transform& t) noexcept {
    const Mat3 r = glm::mat3_cast(t.rotation);
    Mat4       m(1.0f);
    m[0] = Vec4(r[0] * t.scale.x, 0.0f);
    m[1] = Vec4(r[1] * t.scale.y, 0.0f);
    m[2] = Vec4(r[2] * t.scale.z, 0.0f);
    m[3] = Vec4(t.position, 1.0f);
    return m;
}

} // namespace aether::animation
