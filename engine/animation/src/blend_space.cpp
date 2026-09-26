// blend_space.cpp — Bowyer-Watson triangulation of blend-space sample points + runtime weights.
#include "blend_space.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::animation::detail {

namespace {

struct P2 {
    f64 x = 0.0;
    f64 y = 0.0;
};

f64 orient(const P2& a, const P2& b, const P2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// True if p lies strictly inside the circumcircle of triangle (a, b, c).
bool in_circumcircle(const P2& p, const P2& a, const P2& b, const P2& c) {
    const f64 ax = a.x - p.x, ay = a.y - p.y;
    const f64 bx = b.x - p.x, by = b.y - p.y;
    const f64 cx = c.x - p.x, cy = c.y - p.y;
    const f64 det = (ax * ax + ay * ay) * (bx * cy - cx * by) -
                    (bx * bx + by * by) * (ax * cy - cx * ay) +
                    (cx * cx + cy * cy) * (ax * by - bx * ay);
    const f64 o = orient(a, b, c);
    const f64 eps = 1.0e-12;
    return o > 0.0 ? det > eps : det < -eps;
}

f32 dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

} // namespace

BlendTriangulation triangulate_blend_points(std::span<const Vec2> points) {
    BlendTriangulation result;
    const u32          n = static_cast<u32>(points.size());
    if (n < 2) {
        return result;
    }

    std::vector<P2> pts(n + 3);
    f64             minx = std::numeric_limits<f64>::max(), miny = minx;
    f64             maxx = std::numeric_limits<f64>::lowest(), maxy = maxx;
    for (u32 i = 0; i < n; ++i) {
        pts[i] = {static_cast<f64>(points[i].x), static_cast<f64>(points[i].y)};
        minx = std::min(minx, pts[i].x);
        maxx = std::max(maxx, pts[i].x);
        miny = std::min(miny, pts[i].y);
        maxy = std::max(maxy, pts[i].y);
    }
    const f64 span = std::max({maxx - minx, maxy - miny, 1.0e-3});
    const f64 mx = 0.5 * (minx + maxx), my = 0.5 * (miny + maxy);
    pts[n] = {mx - 40.0 * span, my - 20.0 * span};
    pts[n + 1] = {mx, my + 40.0 * span};
    pts[n + 2] = {mx + 40.0 * span, my - 20.0 * span};

    std::vector<std::array<u32, 3>> tris{{n, n + 1, n + 2}};
    std::vector<std::array<u32, 3>> kept;
    std::vector<std::array<u32, 2>> edges;
    for (u32 i = 0; i < n; ++i) {
        kept.clear();
        edges.clear();
        for (const auto& t : tris) {
            if (in_circumcircle(pts[i], pts[t[0]], pts[t[1]], pts[t[2]])) {
                edges.push_back({t[0], t[1]});
                edges.push_back({t[1], t[2]});
                edges.push_back({t[2], t[0]});
            } else {
                kept.push_back(t);
            }
        }
        // The cavity boundary = edges of removed triangles that are not shared by two of them.
        for (usize e = 0; e < edges.size(); ++e) {
            bool shared = false;
            for (usize f = 0; f < edges.size() && !shared; ++f) {
                shared = f != e && ((edges[f][0] == edges[e][1] && edges[f][1] == edges[e][0]) ||
                                    (edges[f][0] == edges[e][0] && edges[f][1] == edges[e][1]));
            }
            if (!shared) {
                kept.push_back({edges[e][0], edges[e][1], i});
            }
        }
        tris.swap(kept);
    }

    for (const auto& t : tris) {
        if (t[0] >= n || t[1] >= n || t[2] >= n) {
            continue;
        }
        const f64 o = orient(pts[t[0]], pts[t[1]], pts[t[2]]);
        if (std::abs(o) <= 1.0e-12 * span * span) {
            continue; // degenerate sliver
        }
        result.triangles.push_back(o > 0.0 ? t : std::array<u32, 3>{t[0], t[2], t[1]});
    }

    if (!result.triangles.empty()) {
        // Boundary edges: used by exactly one triangle.
        for (const auto& t : result.triangles) {
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = t[k], b = t[(k + 1) % 3];
                u32       uses = 0;
                for (const auto& o : result.triangles) {
                    for (u32 m = 0; m < 3; ++m) {
                        const u32 c = o[m], d = o[(m + 1) % 3];
                        if ((c == a && d == b) || (c == b && d == a)) {
                            ++uses;
                        }
                    }
                }
                if (uses == 1) {
                    result.edges.push_back({a, b});
                }
            }
        }
    } else {
        // Collinear: polyline along the principal direction.
        std::vector<u32> order(n);
        for (u32 i = 0; i < n; ++i) {
            order[i] = i;
        }
        const f64 dx = maxx - minx, dy = maxy - miny;
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            return pts[a].x * dx + pts[a].y * dy < pts[b].x * dx + pts[b].y * dy;
        });
        for (u32 i = 0; i + 1 < n; ++i) {
            result.edges.push_back({order[i], order[i + 1]});
        }
    }
    return result;
}

u32 blend2d_weights(std::span<const Vec2> points, const BlendTriangulation& tri, Vec2 p,
                    u32 out_index[3], f32 out_weight[3]) noexcept {
    if (points.empty()) {
        return 0;
    }
    if (points.size() == 1) {
        out_index[0] = 0;
        out_weight[0] = 1.0f;
        return 1;
    }
    for (const auto& t : tri.triangles) {
        const Vec2 a = points[t[0]], v0 = points[t[1]] - a, v1 = points[t[2]] - a, v2 = p - a;
        const f32  d00 = dot2(v0, v0), d01 = dot2(v0, v1), d11 = dot2(v1, v1);
        const f32  d20 = dot2(v2, v0), d21 = dot2(v2, v1);
        const f32  denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < 1.0e-12f) {
            continue;
        }
        const f32 v = (d11 * d20 - d01 * d21) / denom;
        const f32 w = (d00 * d21 - d01 * d20) / denom;
        const f32 u = 1.0f - v - w;
        const f32 eps = -1.0e-5f;
        if (u >= eps && v >= eps && w >= eps) {
            const f32 bary[3] = {std::max(u, 0.0f), std::max(v, 0.0f), std::max(w, 0.0f)};
            const f32 sum = bary[0] + bary[1] + bary[2];
            u32       count = 0;
            for (u32 k = 0; k < 3; ++k) {
                if (bary[k] > 0.0f) {
                    out_index[count] = t[k];
                    out_weight[count] = bary[k] / sum;
                    ++count;
                }
            }
            return count;
        }
    }
    // Outside the hull: interpolate on the nearest boundary edge.
    f32 best = std::numeric_limits<f32>::max();
    u32 bi = 0, bj = 0;
    f32 bt = 0.0f;
    for (const auto& e : tri.edges) {
        const Vec2 a = points[e[0]], ab = points[e[1]] - a;
        const f32  len2 = dot2(ab, ab);
        const f32  t = len2 > 0.0f ? std::clamp(dot2(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const Vec2 q = a + ab * t;
        const f32  d2 = dot2(p - q, p - q);
        if (d2 < best) {
            best = d2;
            bi = e[0];
            bj = e[1];
            bt = t;
        }
    }
    u32 count = 0;
    if (bt < 1.0f) {
        out_index[count] = bi;
        out_weight[count] = 1.0f - bt;
        ++count;
    }
    if (bt > 0.0f) {
        out_index[count] = bj;
        out_weight[count] = bt;
        ++count;
    }
    return count;
}

} // namespace aether::animation::detail
