// blend_space.h (private) — 2D blend-space triangulation and weight computation.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <array>
#include <span>
#include <vector>

namespace aether::animation::detail {

struct BlendTriangulation {
    std::vector<std::array<u32, 3>> triangles; // Delaunay triangles (counter-clockwise)
    std::vector<std::array<u32, 2>> edges;     // boundary edges (hull, or a polyline if collinear)
};

// Delaunay triangulation (Bowyer-Watson) of distinct points. Collinear inputs produce no
// triangles and a sorted polyline of edges; a single point produces nothing.
[[nodiscard]] BlendTriangulation triangulate_blend_points(std::span<const Vec2> points);

// Weights for sample position `p`: barycentric inside a triangle, else linear on the nearest
// boundary edge. Writes up to 3 (point index, weight > 0) pairs; returns the count (>= 1 when
// points is non-empty). Weights sum to 1.
u32 blend2d_weights(std::span<const Vec2> points, const BlendTriangulation& triangulation, Vec2 p,
                    u32 out_index[3], f32 out_weight[3]) noexcept;

} // namespace aether::animation::detail
