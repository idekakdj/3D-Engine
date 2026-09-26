// aether/assets/mesh_processing.h — CPU mesh-processing utilities used by the importers.
//
// Public so procedural-geometry producers (gameplay, editor, tests) generate normals,
// tangents and bounds exactly the way imported content does. All functions operate on
// caller-owned data, allocate only scratch memory, and are thread-safe (re-entrant).
//
// Conventions (match core/geometry.h and glTF 2.0):
//   - triangle lists, counter-clockwise front faces, right-handed space;
//   - Vertex::tangent.w is the bitangent sign: bitangent = cross(normal, tangent.xyz) * w;
//   - UVs have a top-left origin (glTF) and normal maps are +Y-up (OpenGL convention), so
//     the bitangent points towards DECREASING v. This matches MikkTSpace-for-glTF (e.g.
//     three.js computeMikkTSpaceTangents(negateSign = true)) and the Khronos sample viewer.
#pragma once

#include "aether/core/geometry.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <vector>

namespace aether::assets {

// Axis-aligned bounds of all vertices (a zero-size box at the origin when empty).
[[nodiscard]] AABB compute_bounds(Span<const Vertex> vertices);

// Bounds of the vertices referenced by indices[first, first + count). Out-of-range
// indices are ignored.
[[nodiscard]] AABB compute_bounds(Span<const Vertex> vertices,
                                  Span<const u32>    indices,
                                  u32                first,
                                  u32                count);

// Smooth vertex normals: area-weighted face normals accumulated over vertices that share
// a position (welded by exact position, so UV seams and hard-split vertices are smoothed
// together). Vertices touched by no non-degenerate triangle get (0, 1, 0).
void generate_smooth_normals(Span<Vertex> vertices, Span<const u32> indices);

// Flat normals: every triangle gets its own three vertices carrying the face normal. The
// index buffer is rewritten to 0..N-1 and `skin` (if non-null and non-empty) is expanded
// alongside the vertices.
void generate_flat_normals(std::vector<Vertex>&     vertices,
                           std::vector<u32>&        indices,
                           std::vector<SkinVertex>* skin = nullptr);

// Per-vertex tangents by per-triangle UV-gradient accumulation (Lengyel), then
// Gram-Schmidt orthogonalisation against the vertex normal; w = handedness (see file
// comment). Triangles with degenerate UVs contribute nothing; vertices left without a
// usable tangent receive an arbitrary unit tangent perpendicular to the normal (w = +1).
// Requires normals to be present. TODO(assets): switch to MikkTSpace for bit-exact
// agreement with DCC-baked normal maps.
void generate_tangents(Span<Vertex> vertices, Span<const u32> indices);

// Renormalises weights to sum to 1 (joints with zero weight keep their index). A vertex
// whose weights sum to ~0 becomes {joint 0 = 1.0}. Returns false in that fallback case.
bool normalize_skin_weights(SkinVertex& skin_vertex);

// Converts a triangle strip / fan (as an index sequence) to a triangle list with
// consistent counter-clockwise winding. Degenerate triangles are preserved.
[[nodiscard]] std::vector<u32> triangle_strip_to_list(Span<const u32> strip);
[[nodiscard]] std::vector<u32> triangle_fan_to_list(Span<const u32> fan);

} // namespace aether::assets
