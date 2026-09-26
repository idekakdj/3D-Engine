// procedural_mesh.cpp — box / sphere / plane / capsule generators and the built-in asset ids.
#include "aether/gameplay/procedural_mesh.h"

#include "aether/assets/mesh_processing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace aether::gameplay {

namespace {

// Tangents come from the importer's generator so procedural and imported meshes agree on the
// handedness convention; bounds and the single submesh are filled in here.
void finish(assets::MeshData& m) {
    assets::generate_tangents(m.vertices, m.indices);
    AABB bounds;
    bounds.min = Vec3(std::numeric_limits<f32>::max());
    bounds.max = Vec3(-std::numeric_limits<f32>::max());
    for (const Vertex& v : m.vertices) {
        bounds.expand(v.position);
    }
    if (m.vertices.empty()) {
        bounds = AABB{};
    }
    m.bounds = bounds;
    Submesh s;
    s.first_index   = 0;
    s.index_count   = static_cast<u32>(m.indices.size());
    s.material_slot = 0;
    s.bounds        = bounds;
    m.submeshes     = { s };
}

Vertex make_vertex(const Vec3& p, const Vec3& n, const Vec3& t, const Vec2& uv) {
    Vertex v;
    v.position = p;
    v.normal   = n;
    v.tangent  = Vec4(t, 1.0f);
    v.uv0      = uv;
    return v;
}

// A tangent perpendicular to `n` pointing along +U for the generators' UV layouts.
Vec3 fallback_tangent(const Vec3& n) {
    const Vec3 ref = std::abs(n.y) < 0.999f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    return glm::normalize(glm::cross(ref, n));
}

} // namespace

assets::MeshData make_box_mesh(const Vec3& h) {
    assets::MeshData m;
    struct Face {
        Vec3 n, u, v; // normal, +U axis, +V axis (U x V == n for CCW winding)
    };
    const std::array<Face, 6> faces{ {
        { Vec3(1, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0) },
        { Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0) },
        { Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, 0, -1) },
        { Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1) },
        { Vec3(0, 0, 1), Vec3(1, 0, 0), Vec3(0, 1, 0) },
        { Vec3(0, 0, -1), Vec3(-1, 0, 0), Vec3(0, 1, 0) },
    } };
    for (const Face& f : faces) {
        const u32  base   = static_cast<u32>(m.vertices.size());
        const Vec3 center = f.n * h;
        const Vec3 du     = f.u * h;
        const Vec3 dv     = f.v * h;
        m.vertices.push_back(make_vertex(center - du - dv, f.n, f.u, Vec2(0, 1)));
        m.vertices.push_back(make_vertex(center + du - dv, f.n, f.u, Vec2(1, 1)));
        m.vertices.push_back(make_vertex(center + du + dv, f.n, f.u, Vec2(1, 0)));
        m.vertices.push_back(make_vertex(center - du + dv, f.n, f.u, Vec2(0, 0)));
        for (const u32 i : { 0u, 1u, 2u, 2u, 3u, 0u }) {
            m.indices.push_back(base + i);
        }
    }
    finish(m);
    return m;
}

assets::MeshData make_sphere_mesh(f32 radius, u32 segments, u32 rings) {
    segments = std::max(segments, 3u);
    rings    = std::max(rings, 2u);
    assets::MeshData m;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 v     = static_cast<f32>(r) / static_cast<f32>(rings);
        const f32 theta = v * kPi; // 0 at the north pole
        for (u32 s = 0; s <= segments; ++s) {
            const f32  u   = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32  phi = u * kTwoPi;
            const Vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi));
            const Vec3 t(-std::sin(phi), 0.0f, -std::cos(phi));
            m.vertices.push_back(make_vertex(n * radius, n, glm::length(t) > 0.0f ? t : fallback_tangent(n),
                                             Vec2(u, v)));
        }
    }
    const u32 stride = segments + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * stride + s;
            const u32 b = a + stride;
            if (r != 0) {
                m.indices.insert(m.indices.end(), { a, b, a + 1 });
            }
            if (r != rings - 1) {
                m.indices.insert(m.indices.end(), { a + 1, b, b + 1 });
            }
        }
    }
    finish(m);
    return m;
}

assets::MeshData make_plane_mesh(const Vec2& half_size, f32 uv_scale) {
    assets::MeshData m;
    const Vec3       n(0, 1, 0);
    const Vec3       t(1, 0, 0);
    m.vertices = {
        make_vertex(Vec3(-half_size.x, 0, half_size.y), n, t, Vec2(0, uv_scale)),
        make_vertex(Vec3(half_size.x, 0, half_size.y), n, t, Vec2(uv_scale, uv_scale)),
        make_vertex(Vec3(half_size.x, 0, -half_size.y), n, t, Vec2(uv_scale, 0)),
        make_vertex(Vec3(-half_size.x, 0, -half_size.y), n, t, Vec2(0, 0)),
    };
    m.indices = { 0, 1, 2, 2, 3, 0 };
    finish(m);
    return m;
}

assets::MeshData make_capsule_mesh(f32 half_height, f32 radius, u32 segments, u32 cap_rings) {
    segments  = std::max(segments, 3u);
    cap_rings = std::max(cap_rings, 1u);
    assets::MeshData m;
    // Latitude rows: north cap (0..cap_rings), then south cap; the cylinder is the band between
    // the last north row and the first south row.
    const u32 rows = 2 * (cap_rings + 1);
    for (u32 r = 0; r < rows; ++r) {
        const bool north = r <= cap_rings;
        const u32  k     = north ? r : r - (cap_rings + 1);
        const f32  theta = north ? kHalfPi * static_cast<f32>(k) / static_cast<f32>(cap_rings)
                                 : kHalfPi + kHalfPi * static_cast<f32>(k) / static_cast<f32>(cap_rings);
        const f32  y_off = north ? half_height : -half_height;
        const f32  v     = static_cast<f32>(r) / static_cast<f32>(rows - 1);
        for (u32 s = 0; s <= segments; ++s) {
            const f32  u   = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32  phi = u * kTwoPi;
            const Vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi));
            const Vec3 t(-std::sin(phi), 0.0f, -std::cos(phi));
            m.vertices.push_back(make_vertex(n * radius + Vec3(0, y_off, 0), n, t, Vec2(u, v)));
        }
    }
    const u32 stride = segments + 1;
    for (u32 r = 0; r + 1 < rows; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * stride + s;
            const u32 b = a + stride;
            if (r != 0) {
                m.indices.insert(m.indices.end(), { a, b, a + 1 });
            }
            if (r + 2 != rows) {
                m.indices.insert(m.indices.end(), { a + 1, b, b + 1 });
            }
        }
    }
    finish(m);
    return m;
}

AssetId builtin_mesh_id(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Cube: return AssetId::from_string("aether:builtin/cube");
    case BuiltinMesh::Sphere: return AssetId::from_string("aether:builtin/sphere");
    case BuiltinMesh::Plane: return AssetId::from_string("aether:builtin/plane");
    case BuiltinMesh::Capsule: return AssetId::from_string("aether:builtin/capsule");
    case BuiltinMesh::Count: break;
    }
    return {};
}

AssetId default_material_id() { return AssetId::from_string("aether:builtin/default_material"); }

std::optional<BuiltinMesh> builtin_mesh_from_id(const AssetId& id) {
    for (u8 i = 0; i < static_cast<u8>(BuiltinMesh::Count); ++i) {
        const auto mesh = static_cast<BuiltinMesh>(i);
        if (builtin_mesh_id(mesh) == id) {
            return mesh;
        }
    }
    return std::nullopt;
}

assets::MeshData make_builtin_mesh(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Cube: return make_box_mesh(Vec3(0.5f));
    case BuiltinMesh::Sphere: return make_sphere_mesh(0.5f);
    case BuiltinMesh::Plane: return make_plane_mesh(Vec2(5.0f), 5.0f);
    case BuiltinMesh::Capsule: return make_capsule_mesh(0.5f, 0.5f);
    case BuiltinMesh::Count: break;
    }
    return {};
}

assets::MaterialData make_default_material() { return make_builtin_material(BuiltinMaterial::Default); }

const char* builtin_mesh_name(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Cube: return "Cube";
    case BuiltinMesh::Sphere: return "Sphere";
    case BuiltinMesh::Plane: return "Plane";
    case BuiltinMesh::Capsule: return "Capsule";
    case BuiltinMesh::Count: break;
    }
    return "?";
}

const char* builtin_material_name(BuiltinMaterial material) {
    switch (material) {
    case BuiltinMaterial::Default: return "Default";
    case BuiltinMaterial::White: return "White";
    case BuiltinMaterial::Black: return "Black";
    case BuiltinMaterial::Red: return "Red";
    case BuiltinMaterial::Green: return "Green";
    case BuiltinMaterial::Blue: return "Blue";
    case BuiltinMaterial::Yellow: return "Yellow";
    case BuiltinMaterial::Metal: return "Metal";
    case BuiltinMaterial::Gold: return "Gold";
    case BuiltinMaterial::Emissive: return "Emissive";
    case BuiltinMaterial::Count: break;
    }
    return "?";
}

AssetId builtin_material_id(BuiltinMaterial material) {
    if (material == BuiltinMaterial::Default) {
        return default_material_id();
    }
    return AssetId::from_string(std::string("aether:builtin/material/") + builtin_material_name(material));
}

std::optional<BuiltinMaterial> builtin_material_from_id(const AssetId& id) {
    for (u8 i = 0; i < static_cast<u8>(BuiltinMaterial::Count); ++i) {
        const auto m = static_cast<BuiltinMaterial>(i);
        if (builtin_material_id(m) == id) {
            return m;
        }
    }
    return std::nullopt;
}

assets::MaterialData make_builtin_material(BuiltinMaterial material) {
    assets::MaterialData d;
    d.name             = builtin_material_name(material);
    d.metallic_factor  = 0.0f;
    d.roughness_factor = 0.6f;
    switch (material) {
    case BuiltinMaterial::Default: d.base_color_factor = Vec4(0.8f, 0.8f, 0.8f, 1.0f); break;
    case BuiltinMaterial::White: d.base_color_factor = Vec4(0.95f, 0.95f, 0.95f, 1.0f); break;
    case BuiltinMaterial::Black: d.base_color_factor = Vec4(0.04f, 0.04f, 0.04f, 1.0f); break;
    case BuiltinMaterial::Red: d.base_color_factor = Vec4(0.85f, 0.18f, 0.15f, 1.0f); d.roughness_factor = 0.45f; break;
    case BuiltinMaterial::Green: d.base_color_factor = Vec4(0.2f, 0.7f, 0.25f, 1.0f); d.roughness_factor = 0.5f; break;
    case BuiltinMaterial::Blue: d.base_color_factor = Vec4(0.15f, 0.35f, 0.85f, 1.0f); d.roughness_factor = 0.35f; break;
    case BuiltinMaterial::Yellow: d.base_color_factor = Vec4(0.95f, 0.8f, 0.2f, 1.0f); d.roughness_factor = 0.5f; break;
    case BuiltinMaterial::Metal:
        d.base_color_factor = Vec4(0.75f, 0.75f, 0.78f, 1.0f);
        d.metallic_factor   = 1.0f;
        d.roughness_factor  = 0.3f;
        break;
    case BuiltinMaterial::Gold:
        d.base_color_factor = Vec4(1.0f, 0.78f, 0.34f, 1.0f);
        d.metallic_factor   = 1.0f;
        d.roughness_factor  = 0.25f;
        break;
    case BuiltinMaterial::Emissive:
        d.base_color_factor = Vec4(1.0f, 0.6f, 0.3f, 1.0f);
        d.emissive_factor   = Vec3(4.0f, 2.4f, 1.2f);
        break;
    case BuiltinMaterial::Count: break;
    }
    return d;
}

} // namespace aether::gameplay
