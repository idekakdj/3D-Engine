// mesh_processing_tests.cpp — normals, tangents, bounds, skin weights, strip/fan.
#include "aether/assets/mesh_processing.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace aether;
using namespace aether::assets;

namespace {
Vertex vtx(Vec3 p, Vec2 uv = Vec2(0.0f)) {
    Vertex v;
    v.position = p;
    v.uv0 = uv;
    return v;
}
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-5f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}
// A unit quad in the XY plane facing +Z with glTF (top-left origin) UVs.
std::vector<Vertex> quad(bool mirror_u = false) {
    const f32 u0 = mirror_u ? 1.0f : 0.0f, u1 = mirror_u ? 0.0f : 1.0f;
    return { vtx({ 0, 0, 0 }, { u0, 1 }), vtx({ 1, 0, 0 }, { u1, 1 }), vtx({ 1, 1, 0 }, { u1, 0 }),
             vtx({ 0, 1, 0 }, { u0, 0 }) };
}
const std::vector<u32> kQuadIndices{ 0, 1, 2, 0, 2, 3 };
} // namespace

TEST_CASE("mesh_processing: bounds") {
    const std::vector<Vertex> v{ vtx({ -1, 2, 3 }), vtx({ 4, -5, 6 }), vtx({ 0, 0, -7 }) };
    const AABB                all = compute_bounds(v);
    CHECK(near(all.min, { -1, -5, -7 }));
    CHECK(near(all.max, { 4, 2, 6 }));
    const std::vector<u32> idx{ 2, 0, 99 };
    const AABB             sub = compute_bounds(v, idx, 0, 3); // index 99 ignored
    CHECK(near(sub.min, { -1, 0, -7 }));
    CHECK(near(sub.max, { 0, 2, 3 }));
    CHECK(near(compute_bounds(Span<const Vertex>{}).min, { 0, 0, 0 }));
}

TEST_CASE("mesh_processing: smooth normals (planar and welded)") {
    auto v = quad();
    generate_smooth_normals(v, kQuadIndices);
    for (const Vertex& x : v) CHECK(near(x.normal, { 0, 0, 1 }));

    // Two faces of a "roof" with split (unshared) vertices along the ridge: the ridge
    // vertices are welded by position and get the averaged normal.
    std::vector<Vertex> roof{ vtx({ 0, 0, 0 }), vtx({ 1, 0, 0 }), vtx({ 0, 1, 1 }),  // face A (-y,+z)
                              vtx({ 1, 0, 0 }), vtx({ 0, 0, 0 }), vtx({ 0, -1, 1 }) }; // face B
    const std::vector<u32> idx{ 0, 1, 2, 3, 4, 5 };
    generate_smooth_normals(roof, idx);
    const Vec3 ridge = roof[0].normal;
    CHECK(near(ridge, roof[4].normal));
    CHECK(near(ridge, roof[1].normal));
    CHECK(std::fabs(glm::length(ridge) - 1.0f) < 1e-5f);
    CHECK(ridge.x == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("mesh_processing: flat normals un-weld vertices") {
    std::vector<Vertex>     v{ vtx({ 0, 0, 0 }), vtx({ 1, 0, 0 }), vtx({ 0, 1, 0 }), vtx({ 0, 0, 1 }) };
    std::vector<u32>        idx{ 0, 1, 2, 0, 3, 1 }; // two faces sharing edge 0-1
    std::vector<SkinVertex> skin(4);
    skin[3].joints[0] = 7;
    generate_flat_normals(v, idx, &skin);
    REQUIRE(v.size() == 6);
    REQUIRE(skin.size() == 6);
    CHECK(idx == std::vector<u32>{ 0, 1, 2, 3, 4, 5 });
    CHECK(near(v[0].normal, { 0, 0, 1 }));
    CHECK(near(v[3].normal, { 0, 1, 0 })); // (0,0,0) (0,0,1) (1,0,0) is CCW seen from +y
    CHECK(skin[4].joints[0] == 7);
}

TEST_CASE("mesh_processing: tangents follow +u, bitangent follows image-up (-v)") {
    auto v = quad();
    generate_smooth_normals(v, kQuadIndices);
    generate_tangents(v, kQuadIndices);
    for (const Vertex& x : v) {
        CHECK(near(Vec3(x.tangent), { 1, 0, 0 }));
        CHECK(x.tangent.w == 1.0f);
        const Vec3 bitangent = glm::cross(x.normal, Vec3(x.tangent)) * x.tangent.w;
        CHECK(near(bitangent, { 0, 1, 0 })); // v decreases towards +y (top-left UV origin)
    }

    auto m = quad(/*mirror_u=*/true);
    generate_smooth_normals(m, kQuadIndices);
    generate_tangents(m, kQuadIndices);
    for (const Vertex& x : m) {
        CHECK(near(Vec3(x.tangent), { -1, 0, 0 }));
        CHECK(x.tangent.w == -1.0f); // mirrored mapping flips handedness
    }

    // Degenerate UVs: still a unit tangent orthogonal to the normal.
    std::vector<Vertex> flat{ vtx({ 0, 0, 0 }), vtx({ 1, 0, 0 }), vtx({ 0, 1, 0 }) };
    const std::vector<u32> tri{ 0, 1, 2 };
    generate_smooth_normals(flat, tri);
    generate_tangents(flat, tri);
    for (const Vertex& x : flat) {
        CHECK(std::fabs(glm::length(Vec3(x.tangent)) - 1.0f) < 1e-5f);
        CHECK(std::fabs(glm::dot(Vec3(x.tangent), x.normal)) < 1e-5f);
    }
}

TEST_CASE("mesh_processing: MikkTSpace splits mirrored-UV seams") {
    // Two quads side by side sharing the middle edge (vertices 1 and 4). The right quad's u is
    // mirrored (1 -> 0 left to right), so the shared vertices have the same UV on both sides
    // but opposite tangent handedness: MikkTSpace must give them one tangent per side.
    std::vector<Vertex> v{ vtx({ 0, 0, 0 }, { 0, 1 }), vtx({ 1, 0, 0 }, { 1, 1 }), vtx({ 2, 0, 0 }, { 0, 1 }),
                           vtx({ 0, 1, 0 }, { 0, 0 }), vtx({ 1, 1, 0 }, { 1, 0 }), vtx({ 2, 1, 0 }, { 0, 0 }) };
    std::vector<u32> idx{ 0, 1, 4, 0, 4, 3,   // left quad
                          1, 2, 5, 1, 5, 4 }; // right quad (mirrored u)
    std::vector<SkinVertex> skin(6);
    skin[1].joints[0] = 3;
    skin[4].joints[0] = 5;
    generate_smooth_normals(v, idx);

    std::vector<Vertex> unsplit = v;
    generate_tangents(unsplit, idx); // fixed vertex count variant still yields valid frames
    for (const Vertex& x : unsplit) {
        CHECK(std::fabs(glm::length(Vec3(x.tangent)) - 1.0f) < 1e-5f);
        CHECK(std::fabs(x.tangent.w) == 1.0f);
    }

    const u32 added = generate_tangents_mikktspace(v, idx, &skin);
    CHECK(added == 2); // the two seam vertices are split
    REQUIRE(v.size() == 8);
    REQUIRE(skin.size() == 8);
    CHECK(skin[6].joints[0] + skin[7].joints[0] == 8); // copies carry their skin (3 and 5)
    for (usize t = 0; t < idx.size(); ++t) {
        const Vertex& x = v[idx[t]];
        const bool    left = t < 6;
        INFO("corner ", t);
        CHECK(near(Vec3(x.tangent), left ? Vec3(1, 0, 0) : Vec3(-1, 0, 0)));
        CHECK(x.tangent.w == (left ? 1.0f : -1.0f));
        const Vec3 bitangent = glm::cross(x.normal, Vec3(x.tangent)) * x.tangent.w;
        CHECK(near(bitangent, { 0, 1, 0 })); // image-up == +y on both sides
    }
    for (const Vertex& x : v) {
        CHECK(std::fabs(glm::length(Vec3(x.tangent)) - 1.0f) < 1e-5f);
        CHECK(std::fabs(glm::dot(Vec3(x.tangent), x.normal)) < 1e-5f);
    }
    // Geometry is unchanged: the split vertices duplicate positions/UVs.
    CHECK(near(v[idx[6]].position, { 1, 0, 0 }));
    CHECK(idx[0] == 0);

    // A seamless mesh is not split and matches the fixed-count variant exactly.
    auto a = quad();
    generate_smooth_normals(a, kQuadIndices);
    auto             b = a;
    std::vector<u32> bi = kQuadIndices;
    generate_tangents(a, kQuadIndices);
    CHECK(generate_tangents_mikktspace(b, bi) == 0);
    CHECK(bi == kQuadIndices);
    for (usize i = 0; i < a.size(); ++i) CHECK(a[i].tangent == b[i].tangent);
}

TEST_CASE("mesh_processing: skin weight normalisation") {
    SkinVertex s;
    s.weights = Vec4(2.0f, 1.0f, 1.0f, 0.0f);
    CHECK(normalize_skin_weights(s));
    CHECK(s.weights.x == doctest::Approx(0.5f));
    CHECK(s.weights.y == doctest::Approx(0.25f));
    SkinVertex z;
    z.weights = Vec4(0.0f, -1.0f, 0.0f, 0.0f);
    CHECK_FALSE(normalize_skin_weights(z));
    CHECK(z.weights == Vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("mesh_processing: strip and fan triangulation keep winding") {
    const std::vector<u32> strip{ 0, 1, 2, 3, 4 };
    CHECK(triangle_strip_to_list(strip) == std::vector<u32>{ 0, 1, 2, 2, 1, 3, 2, 3, 4 });
    const std::vector<u32> fan{ 0, 1, 2, 3 };
    CHECK(triangle_fan_to_list(fan) == std::vector<u32>{ 0, 1, 2, 0, 2, 3 });
    CHECK(triangle_strip_to_list(std::vector<u32>{ 0, 1 }).empty());
}
