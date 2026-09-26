// shadow_math.h — cascaded shadow map split + stabilized fitting math.
//
// Private header, pure functions (thread-safe), unit-tested.
//   * practical split scheme (log/uniform blend) up to the shadow distance
//   * rotation-invariant bounding sphere per cascade slice, computed in view space
//   * light-space orthographic projection (Vulkan Y-flip, reverse-Z) whose origin is
//     snapped to the shadow-map texel grid, so static geometry never shimmers as the
//     camera translates or rotates
#pragma once

#include "gpu_data.h"

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <array>

namespace aether::renderer {

// View-space far distance of each cascade; entries >= count are left at `max_distance`.
// lambda = 0 -> uniform, 1 -> logarithmic.
[[nodiscard]] std::array<f32, kMaxCascades> compute_cascade_splits(u32 count, f32 near_z,
                                                                   f32 max_distance,
                                                                   f32 lambda = 0.75f);

// The 8 view-space corners of the camera frustum between distances [d0, d1] (positive,
// along -Z). Works for perspective (incl. off-center) and orthographic projections.
[[nodiscard]] std::array<Vec3, 8> frustum_slice_corners_view(const Mat4& proj, f32 d0, f32 d1);

struct BoundingSphere {
    Vec3 center{ 0.0f };
    f32  radius = 0.0f;
};

// Enclosing sphere of a slice, computed in VIEW space so its radius is independent of
// the camera orientation; the center is transformed to world space with `inv_view`.
// The radius is quantized upward (1/16 unit) for extra stability.
[[nodiscard]] BoundingSphere cascade_bounding_sphere(const Mat4& proj, const Mat4& inv_view,
                                                     f32 d0, f32 d1);

struct CascadeMatrices {
    Mat4 view{ 1.0f };
    Mat4 proj{ 1.0f };
    Mat4 view_proj{ 1.0f };
    f32  texel_world = 0.0f; // world-space size of one shadow texel
};

// Light-space fit for one cascade. `light_dir` is the direction the light travels.
// `caster_extent` extends the volume toward the light so casters outside the sphere
// still render (on top of depth clamp, when available).
[[nodiscard]] CascadeMatrices fit_cascade(const BoundingSphere& sphere, const Vec3& light_dir,
                                          u32 shadow_map_size, f32 caster_extent);

// Reverse-Z matrix for a standard [0,1] projection: z' = w - z.
[[nodiscard]] Mat4 reverse_z_matrix();

} // namespace aether::renderer
