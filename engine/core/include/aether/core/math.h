// aether/core/math.h — engine math vocabulary.
//
// FROZEN CONTRACT (ADR-0001). M0-M1 aliases glm (column-major, right-handed) so all
// modules share one vector/matrix type set. A custom SIMD math library may replace
// the backing types post-M1 WITHOUT changing these names — depend only on the
// aliases below, never on glm:: directly in engine code outside this header.
#pragma once

#include "aether/core/types.h"

// glm configuration is applied as PUBLIC compile definitions on aether.core (so every
// TU sees identical settings - no ODR skew). Guards here only cover out-of-tree use.
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#    define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_ENABLE_EXPERIMENTAL
#    define GLM_ENABLE_EXPERIMENTAL
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace aether {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec2 = glm::uvec2;
using UVec3 = glm::uvec3;

using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr f32 kPi      = 3.14159265358979323846f;
inline constexpr f32 kTwoPi   = 2.0f * kPi;
inline constexpr f32 kHalfPi  = 0.5f * kPi;
inline constexpr f32 kDeg2Rad = kPi / 180.0f;
inline constexpr f32 kRad2Deg = 180.0f / kPi;

// Axis-aligned bounding box. Used by culling, physics broadphase, asset bounds.
struct AABB {
    Vec3 min{ 0.0f };
    Vec3 max{ 0.0f };

    [[nodiscard]] Vec3 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] Vec3 extent() const { return (max - min) * 0.5f; }
    [[nodiscard]] bool valid()  const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    void expand(const Vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
    void expand(const AABB& b) { min = glm::min(min, b.min); max = glm::max(max, b.max); }
};

// A rigid transform in TRS form; canonical layout stored on Transform components.
struct Transform {
    Vec3 position{ 0.0f };
    Quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // w,x,y,z identity
    Vec3 scale{ 1.0f };

    [[nodiscard]] Mat4 to_matrix() const {
        Mat4 m = glm::translate(Mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

// Right-handed, zero-to-one-depth perspective (Vulkan clip space).
inline Mat4 perspective(f32 fovy_radians, f32 aspect, f32 z_near, f32 z_far) {
    Mat4 p = glm::perspectiveRH_ZO(fovy_radians, aspect, z_near, z_far);
    p[1][1] *= -1.0f; // flip Y for Vulkan's inverted viewport
    return p;
}

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    return glm::lookAtRH(eye, center, up);
}

} // namespace aether
