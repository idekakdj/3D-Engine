// mesh_processing.cpp — normals, tangents, bounds, skin weights, strip/fan conversion.
#include "aether/assets/mesh_processing.h"

#include "math_util.h"

#include <mikktspace.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace aether::assets {

namespace {

// Exact-position weld key (-0.0 folded into +0.0 so they weld together).
struct PositionKey {
    u32 x, y, z;
    friend bool operator==(const PositionKey&, const PositionKey&) = default;
};
struct PositionKeyHash {
    usize operator()(const PositionKey& k) const noexcept {
        u64 h = 0xcbf29ce484222325ull;
        for (u32 v : { k.x, k.y, k.z }) {
            h ^= v;
            h *= 0x100000001b3ull;
        }
        return static_cast<usize>(h ^ (h >> 32));
    }
};

u32 float_key(f32 f) noexcept {
    return f == 0.0f ? 0u : std::bit_cast<u32>(f);
}

PositionKey position_key(const Vec3& p) noexcept {
    return { float_key(p.x), float_key(p.y), float_key(p.z) };
}

bool triangle_in_range(Span<const u32> indices, usize t, usize vertex_count) noexcept {
    return indices[t] < vertex_count && indices[t + 1] < vertex_count &&
           indices[t + 2] < vertex_count;
}

} // namespace

AABB compute_bounds(Span<const Vertex> vertices) {
    if (vertices.empty()) return AABB{};
    AABB box{ vertices[0].position, vertices[0].position };
    for (const Vertex& v : vertices) box.expand(v.position);
    return box;
}

AABB compute_bounds(Span<const Vertex> vertices, Span<const u32> indices, u32 first, u32 count) {
    AABB box{};
    bool any = false;
    const usize end = std::min<usize>(indices.size(), static_cast<usize>(first) + count);
    for (usize i = first; i < end; ++i) {
        const u32 idx = indices[i];
        if (idx >= vertices.size()) continue;
        if (!any) {
            box = AABB{ vertices[idx].position, vertices[idx].position };
            any = true;
        } else {
            box.expand(vertices[idx].position);
        }
    }
    return box;
}

void generate_smooth_normals(Span<Vertex> vertices, Span<const u32> indices) {
    const usize n = vertices.size();
    if (n == 0) return;

    // Weld by position: every vertex maps to the first vertex with the same position.
    std::vector<u32> rep(n);
    {
        std::unordered_map<PositionKey, u32, PositionKeyHash> weld;
        weld.reserve(n);
        for (usize i = 0; i < n; ++i) {
            rep[i] = weld.try_emplace(position_key(vertices[i].position), static_cast<u32>(i))
                         .first->second;
        }
    }

    std::vector<Vec3> accum(n, Vec3(0.0f));
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!triangle_in_range(indices, t, n)) continue;
        const Vec3& a = vertices[indices[t]].position;
        const Vec3& b = vertices[indices[t + 1]].position;
        const Vec3& c = vertices[indices[t + 2]].position;
        const Vec3 face = detail::cross(b - a, c - a); // |face| = 2 * area -> area weighting
        if (!detail::is_finite(face)) continue;
        for (usize k = 0; k < 3; ++k) accum[rep[indices[t + k]]] += face;
    }
    for (usize i = 0; i < n; ++i) {
        vertices[i].normal = detail::normalize_or(accum[rep[i]], Vec3(0.0f, 1.0f, 0.0f));
    }
}

void generate_flat_normals(std::vector<Vertex>&     vertices,
                           std::vector<u32>&        indices,
                           std::vector<SkinVertex>* skin) {
    const bool         has_skin = skin != nullptr && !skin->empty();
    std::vector<Vertex> out_vertices;
    std::vector<SkinVertex> out_skin;
    std::vector<u32>    out_indices;
    out_vertices.reserve(indices.size());
    out_indices.reserve(indices.size());
    if (has_skin) out_skin.reserve(indices.size());

    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!triangle_in_range(indices, t, vertices.size())) continue;
        const Vec3 face = detail::cross(vertices[indices[t + 1]].position - vertices[indices[t]].position,
                                        vertices[indices[t + 2]].position - vertices[indices[t]].position);
        const Vec3 normal = detail::normalize_or(face, Vec3(0.0f, 1.0f, 0.0f));
        for (usize k = 0; k < 3; ++k) {
            Vertex v = vertices[indices[t + k]];
            v.normal = normal;
            out_indices.push_back(static_cast<u32>(out_vertices.size()));
            out_vertices.push_back(v);
            if (has_skin) out_skin.push_back((*skin)[indices[t + k]]);
        }
    }
    vertices = std::move(out_vertices);
    indices = std::move(out_indices);
    if (has_skin) *skin = std::move(out_skin);
}

namespace {

// ---------------------------------------------------------------------------
// MikkTSpace adapter
// ---------------------------------------------------------------------------
struct CornerTangent {
    Vec3 tangent{ 0.0f };
    f32  sign = 1.0f;
    bool set = false;
};

struct MikkMesh {
    Span<const Vertex>         vertices;
    std::vector<u32>           corners; // valid triangles only, 3 original vertex indices each
    std::vector<CornerTangent> result;  // parallel to `corners`
};

const Vertex& mikk_vertex(const SMikkTSpaceContext* ctx, int face, int vert) {
    const auto* m = static_cast<const MikkMesh*>(ctx->m_pUserData);
    return m->vertices[m->corners[static_cast<usize>(face) * 3 + static_cast<usize>(vert)]];
}

int mikk_num_faces(const SMikkTSpaceContext* ctx) {
    return static_cast<int>(static_cast<const MikkMesh*>(ctx->m_pUserData)->corners.size() / 3);
}
int mikk_num_face_vertices(const SMikkTSpaceContext*, const int) { return 3; }
void mikk_position(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const Vec3& p = mikk_vertex(ctx, face, vert).position;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}
void mikk_normal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const Vec3 n = detail::normalize_or(mikk_vertex(ctx, face, vert).normal, Vec3(0.0f, 1.0f, 0.0f));
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}
void mikk_texcoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const Vec2& uv = mikk_vertex(ctx, face, vert).uv0;
    out[0] = uv.x;
    out[1] = 1.0f - uv.y; // glTF top-left origin -> MikkTSpace bottom-left, as Blender's glTF exporter
}
void mikk_set_basic(const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int face,
                    const int vert) {
    auto*          m = static_cast<MikkMesh*>(ctx->m_pUserData);
    CornerTangent& c = m->result[static_cast<usize>(face) * 3 + static_cast<usize>(vert)];
    c.tangent = Vec3(tangent[0], tangent[1], tangent[2]);
    c.sign = sign < 0.0f ? -1.0f : 1.0f;
    c.set = true;
}

// Runs MikkTSpace over the in-range triangles; per-corner results land in the returned mesh.
MikkMesh run_mikktspace(Span<const Vertex> vertices, Span<const u32> indices) {
    MikkMesh mesh;
    mesh.vertices = vertices;
    mesh.corners.reserve(indices.size());
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!triangle_in_range(indices, t, vertices.size())) continue;
        mesh.corners.insert(mesh.corners.end(), { indices[t], indices[t + 1], indices[t + 2] });
    }
    mesh.result.resize(mesh.corners.size());
    if (mesh.corners.empty()) return mesh;

    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = &mikk_num_faces;
    iface.m_getNumVerticesOfFace = &mikk_num_face_vertices;
    iface.m_getPosition = &mikk_position;
    iface.m_getNormal = &mikk_normal;
    iface.m_getTexCoord = &mikk_texcoord;
    iface.m_setTSpaceBasic = &mikk_set_basic;
    iface.m_setTSpace = nullptr;
    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &mesh;
    if (!genTangSpaceDefault(&ctx)) {
        for (CornerTangent& c : mesh.result) c.set = false; // allocation failure: use fallbacks
    }
    return mesh;
}

// A unit tangent perpendicular to the normal from a MikkTSpace result (or a fallback).
Vec4 finish_tangent(const Vec3& raw_normal, const Vec3& tangent, f32 sign) {
    const Vec3 normal = detail::normalize_or(raw_normal, Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 projected = tangent - normal * detail::dot(normal, tangent);
    const f32  len = detail::length(projected);
    if (len > 1e-6f && std::isfinite(len)) return Vec4(projected / len, sign);
    return Vec4(detail::any_perpendicular(normal), 1.0f);
}

} // namespace

u32 generate_tangents_mikktspace(std::vector<Vertex>&     vertices,
                                 std::vector<u32>&        indices,
                                 std::vector<SkinVertex>* skin) {
    const usize n = vertices.size();
    if (n == 0) return 0;
    const bool     has_skin = skin != nullptr && skin->size() == n;
    const MikkMesh mesh = run_mikktspace(vertices, indices);

    // Per original vertex: the distinct MikkTSpace tangents of its corners and the vertex
    // each one lives in (the first keeps the original index).
    struct Variant {
        Vec4 tangent;
        u32  index;
    };
    std::vector<std::vector<Variant>> variants(n);
    std::vector<u32>                  corner_index(mesh.corners.size());
    for (usize c = 0; c < mesh.corners.size(); ++c) {
        const u32            v = mesh.corners[c];
        const CornerTangent& ct = mesh.result[c];
        const Vec4 t = finish_tangent(vertices[v].normal, ct.set ? ct.tangent : Vec3(0.0f), ct.set ? ct.sign : 1.0f);
        u32  target = v;
        bool found = false;
        for (const Variant& var : variants[v]) {
            if (var.tangent == t) {
                target = var.index;
                found = true;
                break;
            }
        }
        if (!found) {
            if (!variants[v].empty()) {
                target = static_cast<u32>(vertices.size());
                const Vertex copy = vertices[v];
                vertices.push_back(copy);
                if (has_skin) {
                    const SkinVertex skin_copy = (*skin)[v];
                    skin->push_back(skin_copy);
                }
            }
            variants[v].push_back({ t, target });
            vertices[target].tangent = t;
        }
        corner_index[c] = target;
    }
    // Vertices no valid triangle references still get a well-formed tangent.
    for (usize v = 0; v < n; ++v) {
        if (variants[v].empty()) vertices[v].tangent = finish_tangent(vertices[v].normal, Vec3(0.0f), 1.0f);
    }
    // Rewrite the index buffer (same traversal order as run_mikktspace; out-of-range
    // triangles are left untouched).
    usize c = 0;
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] >= n || indices[t + 1] >= n || indices[t + 2] >= n) continue;
        for (usize k = 0; k < 3; ++k) indices[t + k] = corner_index[c++];
    }
    return static_cast<u32>(vertices.size() - n);
}

void generate_tangents(Span<Vertex> vertices, Span<const u32> indices) {
    const usize n = vertices.size();
    if (n == 0) return;
    const MikkMesh mesh = run_mikktspace(vertices, indices);

    // Without re-indexing, a vertex whose corners disagree takes the average tangent of its
    // majority handedness.
    std::vector<Vec3> sum_pos(n, Vec3(0.0f)), sum_neg(n, Vec3(0.0f));
    std::vector<u32>  count_pos(n, 0), count_neg(n, 0);
    for (usize c = 0; c < mesh.corners.size(); ++c) {
        const CornerTangent& ct = mesh.result[c];
        if (!ct.set) continue;
        const u32 v = mesh.corners[c];
        if (ct.sign < 0.0f) {
            sum_neg[v] += ct.tangent;
            ++count_neg[v];
        } else {
            sum_pos[v] += ct.tangent;
            ++count_pos[v];
        }
    }
    for (usize v = 0; v < n; ++v) {
        const bool negative = count_neg[v] > count_pos[v];
        vertices[v].tangent = finish_tangent(vertices[v].normal, negative ? sum_neg[v] : sum_pos[v],
                                             negative ? -1.0f : 1.0f);
    }
}

bool normalize_skin_weights(SkinVertex& sv) {
    f32 w[4] = { sv.weights.x, sv.weights.y, sv.weights.z, sv.weights.w };
    f32 sum = 0.0f;
    for (f32& x : w) {
        if (!(x > 0.0f) || !std::isfinite(x)) x = 0.0f;
        sum += x;
    }
    if (!(sum > 1e-8f)) {
        sv.weights = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        return false;
    }
    const f32 inv = 1.0f / sum;
    sv.weights = Vec4(w[0] * inv, w[1] * inv, w[2] * inv, w[3] * inv);
    return true;
}

std::vector<u32> triangle_strip_to_list(Span<const u32> strip) {
    std::vector<u32> list;
    if (strip.size() < 3) return list;
    list.reserve((strip.size() - 2) * 3);
    for (usize i = 0; i + 2 < strip.size(); ++i) {
        if ((i & 1u) == 0) {
            list.insert(list.end(), { strip[i], strip[i + 1], strip[i + 2] });
        } else {
            list.insert(list.end(), { strip[i + 1], strip[i], strip[i + 2] });
        }
    }
    return list;
}

std::vector<u32> triangle_fan_to_list(Span<const u32> fan) {
    std::vector<u32> list;
    if (fan.size() < 3) return list;
    list.reserve((fan.size() - 2) * 3);
    for (usize i = 1; i + 1 < fan.size(); ++i) {
        list.insert(list.end(), { fan[0], fan[i], fan[i + 1] });
    }
    return list;
}

} // namespace aether::assets
