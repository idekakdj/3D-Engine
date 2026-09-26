// aether/gameplay/procedural_mesh.h — generated meshes and the built-in asset ids.
//
// Generators return assets::MeshData in the engine's canonical layout (48-byte Vertex with
// normals, tangents and UVs; counter-clockwise front faces; one submesh), so the render bridge
// uploads them exactly like imported meshes.
//
// Built-in assets have stable, serializable AssetIds (AssetId::from_string("aether:builtin/...")).
// RenderResourceCache resolves them without the asset database, so scenes may reference
// e.g. builtin_mesh_id(BuiltinMesh::Cube) directly (editor "Create > Cube", samples, tests).
//
// Thread-safety: pure functions; thread-safe.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <optional>

namespace aether::gameplay {

// Axis-aligned box centred on the origin (24 vertices: hard edges, per-face UVs 0..1).
[[nodiscard]] assets::MeshData make_box_mesh(const Vec3& half_extents = Vec3(0.5f));
// UV sphere centred on the origin. segments >= 3 (longitude), rings >= 2 (latitude).
[[nodiscard]] assets::MeshData make_sphere_mesh(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16);
// Plane in XZ facing +Y, centred on the origin; UVs span [0, uv_scale].
[[nodiscard]] assets::MeshData make_plane_mesh(const Vec2& half_size = Vec2(5.0f), f32 uv_scale = 1.0f);
// Capsule along +Y (cylinder half height + hemispherical caps), centred on the origin.
[[nodiscard]] assets::MeshData make_capsule_mesh(f32 half_height = 0.5f, f32 radius = 0.5f,
                                                 u32 segments = 24, u32 cap_rings = 8);

enum class BuiltinMesh : u8 { Cube = 0, Sphere, Plane, Capsule, Count };

// Stable ids of the built-in meshes and of the default (light grey, rough dielectric) material.
[[nodiscard]] AssetId builtin_mesh_id(BuiltinMesh mesh);
[[nodiscard]] AssetId default_material_id();
// The built-in mesh for `id`, if it is one (unit sizes: cube 1 m, sphere d = 1 m, plane 10 m,
// capsule 2 m tall).
[[nodiscard]] std::optional<BuiltinMesh> builtin_mesh_from_id(const AssetId& id);
[[nodiscard]] assets::MeshData          make_builtin_mesh(BuiltinMesh mesh);
[[nodiscard]] assets::MaterialData      make_default_material();

} // namespace aether::gameplay
