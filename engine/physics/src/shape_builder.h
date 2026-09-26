// shape_builder.h (private) — ColliderComponent / character settings -> Jolt shapes.
#pragma once

#include "jolt_common.h"

#include "aether/core/types.h"
#include "aether/physics/components.h"

namespace aether::physics::detail {

struct BuiltShape {
    JPH::RefConst<JPH::Shape> shape; // final body shape (offset/rotation + scale applied)
    JPH::RefConst<JPH::Shape> base;  // undecorated, unscaled primitive (debug drawing)
    String                    error; // non-empty on failure (shape is null)
};

// Builds the body shape for `collider` with the entity's world `scale` baked in.
[[nodiscard]] BuiltShape build_collider_shape(const ColliderComponent& collider, const Vec3& scale);

// Upright capsule whose bottom touches the local origin (character "feet" convention).
[[nodiscard]] JPH::RefConst<JPH::Shape> build_character_shape(f32 half_height, f32 radius);

// True if `scale` differs from (1,1,1) enough to require a ScaledShape.
[[nodiscard]] bool needs_scaling(const Vec3& scale);

} // namespace aether::physics::detail
