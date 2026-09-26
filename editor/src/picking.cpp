// picking.cpp — viewport ray picking (see picking.h).
#include "aether/editor/picking.h"

#include "aether/gameplay/render_bridge.h"
#include "aether/scene/components.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <algorithm>
#include <limits>

namespace aether::editor {

Ray make_pick_ray(const Mat4& view, const Mat4& proj, const Vec2& uv) {
    const Mat4 inv  = glm::inverse(proj * view);
    const Vec2 ndc  = uv * 2.0f - 1.0f; // perspective() flips Y: top of the screen is ndc.y = -1
    Vec4       near = inv * Vec4(ndc, 0.0f, 1.0f);
    Vec4       far  = inv * Vec4(ndc, 1.0f, 1.0f);
    near /= near.w;
    far /= far.w;
    Ray r;
    r.origin    = Vec3(near);
    const Vec3 d = Vec3(far) - Vec3(near);
    const f32  len = glm::length(d);
    r.direction = len > 0.0f ? d / len : Vec3(0, 0, -1);
    return r;
}

std::optional<f32> ray_aabb(const Ray& ray, const AABB& box) {
    f32 tmin = 0.0f;
    f32 tmax = std::numeric_limits<f32>::max();
    for (int a = 0; a < 3; ++a) {
        const f32 o = ray.origin[a];
        const f32 d = ray.direction[a];
        if (std::abs(d) < 1e-12f) {
            if (o < box.min[a] || o > box.max[a]) {
                return std::nullopt;
            }
            continue;
        }
        f32 t0 = (box.min[a] - o) / d;
        f32 t1 = (box.max[a] - o) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) {
            return std::nullopt;
        }
    }
    return tmin;
}

std::optional<PickHit> pick_entity(const World& world, gameplay::RenderResourceCache& cache, const Ray& ray) {
    std::optional<PickHit> best;
    for (const auto [e, mr] : world.registry().view<const MeshRendererComponent>().each()) {
        if (!scene::is_visible_in_hierarchy(world, e)) {
            continue;
        }
        const auto* mesh = cache.mesh(mr.mesh);
        if (mesh == nullptr) {
            continue;
        }
        const AABB bounds = gameplay::transform_aabb(world.world_matrix(e), mesh->bounds);
        if (const auto t = ray_aabb(ray, bounds); t && (!best || *t < best->distance)) {
            best = PickHit{ e, *t };
        }
    }
    return best;
}

} // namespace aether::editor
