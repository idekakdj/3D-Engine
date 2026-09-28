// render_math.h — pure CPU math shared by the renderer passes and unit tests.
//
// Private header. Everything here is side-effect free and thread-safe:
//   * reverse-Z projection conversion + clip-space jitter
//   * Halton low-discrepancy sequence (TAA jitter)
//   * frustum planes + AABB/sphere tests (CPU culling)
//   * clustered-shading index math (mirrors shaders/renderer/clusters.glsl)
#pragma once

#include "gpu_data.h"

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <array>
#include <span>
#include <vector>

namespace aether::renderer {

// ---------------------------------------------------------------------------
// Projection helpers
// ---------------------------------------------------------------------------

// NDC depth of the point on the near plane (view-space z = -near) under `proj`.
[[nodiscard]] f32 ndc_depth_at(const Mat4& proj, f32 view_distance);

// True when `proj` already maps the near plane to depth 1 (reverse-Z).
[[nodiscard]] bool is_reverse_z(const Mat4& proj, f32 near_z);

// Converts a standard [0,1] (near=0, far=1) projection into reverse-Z (near=1, far=0)
// by pre-multiplying with z' = w - z. Returns `proj` unchanged if it is already reversed.
[[nodiscard]] Mat4 to_reverse_z(const Mat4& proj, f32 near_z);

// Applies a sub-pixel jitter (in NDC units) to any projection: clip.xy += jitter * clip.w.
// Works for perspective and orthographic matrices alike.
[[nodiscard]] Mat4 apply_clip_jitter(const Mat4& proj, Vec2 jitter_ndc);

// Radical inverse of `index` in `base` (Halton sequence element), in [0, 1).
[[nodiscard]] f32 halton(u32 index, u32 base);

// TAA jitter for frame `frame` (8-sample Halton(2,3) cycle), in pixels within [-0.5, 0.5).
[[nodiscard]] Vec2 taa_jitter_pixels(u64 frame, u32 sample_count = 8);

// ---------------------------------------------------------------------------
// Culling
// ---------------------------------------------------------------------------

// Plane: dot(n, p) + d >= 0 means "inside". Stored as (n.x, n.y, n.z, d), normalized.
struct Frustum {
    std::array<Vec4, 6> planes{}; // left, right, bottom, top, near, far
    u32                 plane_count = 6;
};

// Gribb-Hartmann extraction for Vulkan clip space (0 <= z <= w), perspective or
// orthographic. `reverse_z` tells which depth plane faces the eye (reverse-Z: z == w at
// the near plane). `skip_near_plane` drops that plane so shadow casters between the
// light and the cascade volume are kept (they are pancaked by depth clamp / near fit).
[[nodiscard]] Frustum extract_frustum(const Mat4& view_proj, bool reverse_z = true,
                                      bool skip_near_plane = false);

// Conservative AABB-vs-frustum test (true = possibly visible).
[[nodiscard]] bool frustum_intersects_aabb(const Frustum& f, const AABB& box);
[[nodiscard]] bool frustum_intersects_sphere(const Frustum& f, const Vec3& center, f32 radius);

// Transforms a local AABB by an affine matrix (Arvo's method; exact for the box corners).
[[nodiscard]] AABB transform_aabb(const AABB& local, const Mat4& m);

// True for the default-constructed/degenerate box (min == max == 0) callers leave unset.
[[nodiscard]] bool aabb_is_unset(const AABB& b);

// ---------------------------------------------------------------------------
// Clusters (16x9x24 froxels, exponential depth slices)
// ---------------------------------------------------------------------------
struct ClusterParams {
    f32 z_scale = 0.0f; // slice = floor(log(view_z) * z_scale - z_bias)
    f32 z_bias = 0.0f;
    u32 tile_w = 1;     // pixels per tile
    u32 tile_h = 1;
};

[[nodiscard]] ClusterParams make_cluster_params(UVec2 viewport, f32 near_z, f32 far_z);

// Depth slice for a positive view-space distance (clamped to [0, kClusterZ-1]).
[[nodiscard]] u32 cluster_slice(f32 view_z, const ClusterParams& p);

// View-space near distance of slice `s` (s == kClusterZ gives far_z).
[[nodiscard]] f32 cluster_slice_near(u32 s, f32 near_z, f32 far_z);

// Linear cluster index; x fastest, then y, then z (matches clusters.glsl).
[[nodiscard]] constexpr u32 cluster_index(u32 x, u32 y, u32 z) noexcept {
    return x + kClusterX * (y + kClusterY * z);
}

// Cluster containing a pixel at `frag` (pixels) with positive view depth `view_z`.
[[nodiscard]] u32 cluster_index_for(Vec2 frag, f32 view_z, const ClusterParams& p);

// Bounding sphere of a spot light cone (apex, unit direction, range, cos(outer)).
struct Sphere {
    Vec3 center{ 0.0f };
    f32  radius = 0.0f;
};
[[nodiscard]] Sphere spot_bounding_sphere(const Vec3& apex, const Vec3& dir, f32 range,
                                          f32 cos_outer);

// ---- point-light shadows (ADR-0015) ----
// Cube face `f` (0..5 = +X, -X, +Y, -Y, +Z, -Z): view direction + up vector of its shadow view.
struct PointShadowFace {
    Vec3 forward{ 1.0f, 0.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
};
[[nodiscard]] PointShadowFace point_shadow_face(u32 f);
// Face whose view contains `dir` (light -> receiver) by its major axis; mirrors
// shadows.glsl::ae_point_shadow_face (ties go to X, then Y).
[[nodiscard]] u32 point_shadow_face_for(const Vec3& dir);
// Texels kept around the 90-degree core of each face so the shader's PCF footprint (normal
// offset + 3x3 taps + bilinear quad) never leaves the face it selected.
inline constexpr u32 kPointShadowMarginTexels = 5;
// tan(fov / 2) of a face view of `size` texels (slightly above 1).
[[nodiscard]] f32 point_shadow_tan_half_fov(u32 size);

// ---------------------------------------------------------------------------
// Hi-Z occlusion (ADR-0009). CPU reference of hiz_build.comp + gpu_cull.comp::occluded().
// Reverse-Z: nearer = LARGER depth; every Hi-Z texel stores the MIN (farthest) depth of the
// depth pixels it covers, so "nearest < texel" proves the whole region hides the object.
// ---------------------------------------------------------------------------
// Mip 0 = largest power of two <= the depth extent (per axis); full chain down to 1x1.
[[nodiscard]] UVec2 hiz_mip0_size(UVec2 depth_size);
[[nodiscard]] u32   hiz_mip_count(UVec2 mip0);

struct HiZPyramid {
    std::vector<UVec2>            sizes;
    std::vector<std::vector<f32>> levels; // row-major
};
[[nodiscard]] HiZPyramid build_hiz(std::span<const f32> depth, UVec2 depth_size);

// Conservative test of the inclusive depth-pixel rectangle [px0, px1] whose nearest depth is
// `nearest`: true only if every covered depth pixel is strictly nearer (larger) than it.
[[nodiscard]] bool hiz_occluded(const HiZPyramid& hiz, UVec2 depth_size, UVec2 px0, UVec2 px1, f32 nearest);

// Screen rectangle (inclusive depth pixels) + nearest reverse-Z depth of a world AABB under
// `view_proj`. False if the box crosses the camera plane (no bounded rectangle).
[[nodiscard]] bool project_aabb_rect(const Mat4& view_proj, const AABB& box, UVec2 viewport, UVec2& px0,
                                     UVec2& px1, f32& nearest);

// Physically based punctual-light distance attenuation (inverse square with a smooth
// window reaching zero at `range`). Mirrors brdf.glsl::distance_attenuation.
[[nodiscard]] f32 distance_attenuation(f32 distance, f32 range);

} // namespace aether::renderer
