// meshlet_tests.cpp — CPU meshlet building for the mesh-shader path (ADR-0010).
#include "meshlets.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace aether;
using namespace aether::renderer;

namespace {

// An n x n grid of quads in the XZ plane (normal +Y, CCW seen from above), offset by `y`.
void add_grid(std::vector<Vertex>& v, std::vector<u32>& idx, u32 n, f32 y, bool flip) {
    const u32 base = static_cast<u32>(v.size());
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            Vertex vert;
            vert.position = Vec3(f32(x), y, f32(z));
            vert.normal = Vec3(0.0f, flip ? -1.0f : 1.0f, 0.0f);
            v.push_back(vert);
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = base + z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            // CCW seen from +Y: a, c, b and b, c, d (x right, z toward the viewer).
            std::array<u32, 6> q = { a, c, b, b, c, d };
            if (flip) {
                std::swap(q[1], q[2]);
                std::swap(q[4], q[5]);
            }
            idx.insert(idx.end(), q.begin(), q.end());
        }
    }
}

// Canonical form of a triangle that keeps its winding: rotate so the smallest index is first.
std::array<u32, 3> canon(u32 a, u32 b, u32 c) {
    if (b < a && b < c) return { b, c, a };
    if (c < a && c < b) return { c, a, b };
    return { a, b, c };
}

std::vector<std::array<u32, 3>> source_tris(const std::vector<u32>& idx, const Submesh& sm) {
    std::vector<std::array<u32, 3>> t;
    for (u32 i = 0; i < sm.index_count; i += 3) {
        t.push_back(canon(idx[sm.first_index + i], idx[sm.first_index + i + 1], idx[sm.first_index + i + 2]));
    }
    std::sort(t.begin(), t.end());
    return t;
}

std::vector<std::array<u32, 3>> meshlet_tris(const MeshletBuild& b, const MeshletBuild::Range& r) {
    std::vector<std::array<u32, 3>> t;
    for (u32 m = r.first; m < r.first + r.count; ++m) {
        const GpuMeshlet& g = b.meshlets[m];
        for (u32 k = 0; k < g.triangle_count; ++k) {
            const u32 p = b.words[g.triangle_offset + k];
            const u32 i0 = p & 0xFFu, i1 = (p >> 8) & 0xFFu, i2 = (p >> 16) & 0xFFu;
            REQUIRE(i0 < g.vertex_count);
            REQUIRE(i1 < g.vertex_count);
            REQUIRE(i2 < g.vertex_count);
            t.push_back(canon(b.words[g.vertex_offset + i0], b.words[g.vertex_offset + i1], b.words[g.vertex_offset + i2]));
        }
    }
    std::sort(t.begin(), t.end());
    return t;
}

// The task shader's backface test (meshlet.task): culled when the camera sees only back faces.
bool cone_culled(const GpuMeshlet& g, Vec3 camera) {
    if (g.cone_cutoff >= 1.0f) return false;
    return glm::dot(glm::normalize(g.cone_apex - camera), g.cone_axis) >= g.cone_cutoff - 1e-3f;
}

} // namespace

TEST_CASE("meshlets: every triangle survives exactly once with its winding, per submesh") {
    std::vector<Vertex> v;
    std::vector<u32>    idx;
    add_grid(v, idx, 24, 0.0f, false);                // 1152 triangles
    const u32 first2 = static_cast<u32>(idx.size());
    add_grid(v, idx, 10, 5.0f, true);                 // 200 triangles, facing down
    const std::array<Submesh, 2> subs = { Submesh{ 0, first2, 0, {} },
                                          Submesh{ first2, static_cast<u32>(idx.size()) - first2, 0, {} } };
    const MeshletBuild b = build_meshlets(v, idx, subs);
    REQUIRE(b.submeshes.size() == 2);
    CHECK(b.submeshes[0].first == 0);
    CHECK(b.submeshes[1].first == b.submeshes[0].count);
    CHECK(b.submeshes[0].count + b.submeshes[1].count == b.meshlets.size());
    CHECK(b.submeshes[0].count >= 1152 / kMeshletMaxTriangles);
    for (usize s = 0; s < 2; ++s) {
        CHECK(meshlet_tris(b, b.submeshes[s]) == source_tris(idx, subs[s]));
    }
    CHECK(b.gpu_bytes() == b.meshlets.size() * 64 + b.words.size() * 4);
}

TEST_CASE("meshlets: limits, word ranges and bounding spheres") {
    std::vector<Vertex> v;
    std::vector<u32>    idx;
    add_grid(v, idx, 40, 0.0f, false);
    const std::array<Submesh, 1> subs = { Submesh{ 0, static_cast<u32>(idx.size()), 0, {} } };
    const MeshletBuild b = build_meshlets(v, idx, subs);
    REQUIRE_FALSE(b.meshlets.empty());
    for (const GpuMeshlet& g : b.meshlets) {
        CHECK(g.vertex_count > 0);
        CHECK(g.vertex_count <= kMeshletMaxVertices);
        CHECK(g.triangle_count > 0);
        CHECK(g.triangle_count <= kMeshletMaxTriangles);
        CHECK(g.triangle_offset == g.vertex_offset + g.vertex_count); // vertices then triangles
        CHECK(static_cast<u64>(g.triangle_offset) + g.triangle_count <= b.words.size());
        for (u32 k = 0; k < g.vertex_count; ++k) {
            const u32 vi = b.words[g.vertex_offset + k];
            REQUIRE(vi < v.size());
            CHECK(glm::length(v[vi].position - g.center) <= g.radius * 1.0001f + 1e-4f);
        }
    }
}

TEST_CASE("meshlets: normal cones match the task shader's backface test") {
    std::vector<Vertex> v;
    std::vector<u32>    idx;
    add_grid(v, idx, 8, 0.0f, false); // faces +Y
    const std::array<Submesh, 1> subs = { Submesh{ 0, static_cast<u32>(idx.size()), 0, {} } };
    const MeshletBuild b = build_meshlets(v, idx, subs);
    REQUIRE_FALSE(b.meshlets.empty());
    for (const GpuMeshlet& g : b.meshlets) {
        REQUIRE(g.cone_cutoff < 1.0f); // a flat patch has a tight cone
        CHECK_FALSE(cone_culled(g, Vec3(4.0f, 10.0f, 4.0f)));  // above: front faces visible
        CHECK(cone_culled(g, Vec3(4.0f, -10.0f, 4.0f)));        // below: back faces only
    }
}

TEST_CASE("meshlets: empty and out-of-range submeshes produce no meshlets") {
    std::vector<Vertex> v;
    std::vector<u32>    idx;
    add_grid(v, idx, 2, 0.0f, false);
    const std::array<Submesh, 2> subs = { Submesh{ 0, 0, 0, {} }, Submesh{ 6, 9999, 0, {} } };
    const MeshletBuild b = build_meshlets(v, idx, subs);
    CHECK(b.meshlets.empty());
    CHECK(b.submeshes[0].count == 0);
    CHECK(b.submeshes[1].count == 0);
}
