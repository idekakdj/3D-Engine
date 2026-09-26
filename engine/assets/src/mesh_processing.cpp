// mesh_processing.cpp — normals, tangents, bounds, skin weights, strip/fan conversion.
#include "aether/assets/mesh_processing.h"

#include "math_util.h"

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

void generate_tangents(Span<Vertex> vertices, Span<const u32> indices) {
    const usize n = vertices.size();
    if (n == 0) return;
    std::vector<Vec3> tan_u(n, Vec3(0.0f)); // accumulated dP/du
    std::vector<Vec3> tan_v(n, Vec3(0.0f)); // accumulated dP/d(-v) (image "up", see header)

    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!triangle_in_range(indices, t, n)) continue;
        const Vertex& v0 = vertices[indices[t]];
        const Vertex& v1 = vertices[indices[t + 1]];
        const Vertex& v2 = vertices[indices[t + 2]];
        const Vec3 e1 = v1.position - v0.position;
        const Vec3 e2 = v2.position - v0.position;
        const f32  s1 = v1.uv0.x - v0.uv0.x;
        const f32  s2 = v2.uv0.x - v0.uv0.x;
        const f32  t1 = -(v1.uv0.y - v0.uv0.y); // flip v: top-left UV origin -> +Y-up normal maps
        const f32  t2 = -(v2.uv0.y - v0.uv0.y);
        const f32  det = s1 * t2 - s2 * t1;
        if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) continue; // degenerate UVs
        const f32  inv = 1.0f / det;
        const Vec3 sdir = (e1 * t2 - e2 * t1) * inv;
        const Vec3 tdir = (e2 * s1 - e1 * s2) * inv;
        if (!detail::is_finite(sdir) || !detail::is_finite(tdir)) continue;
        for (usize k = 0; k < 3; ++k) {
            tan_u[indices[t + k]] += sdir;
            tan_v[indices[t + k]] += tdir;
        }
    }

    for (usize i = 0; i < n; ++i) {
        const Vec3 normal = detail::normalize_or(vertices[i].normal, Vec3(0.0f, 1.0f, 0.0f));
        // Gram-Schmidt: remove the normal component.
        const Vec3 projected = tan_u[i] - normal * detail::dot(normal, tan_u[i]);
        const f32  len = detail::length(projected);
        Vec3       tangent;
        f32        w = 1.0f;
        if (len > 1e-6f * std::max(1.0f, detail::length(tan_u[i])) && std::isfinite(len)) {
            tangent = projected / len;
            w = detail::dot(detail::cross(normal, tangent), tan_v[i]) < 0.0f ? -1.0f : 1.0f;
        } else {
            tangent = detail::any_perpendicular(normal);
        }
        vertices[i].tangent = Vec4(tangent, w);
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
