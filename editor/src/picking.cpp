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

std::vector<Entity> entities_in_rect(const World& world, gameplay::RenderResourceCache& cache, const Mat4& view_proj,
                                     const Vec2& uv_a, const Vec2& uv_b) {
    const Vec2          lo = glm::min(uv_a, uv_b);
    const Vec2          hi = glm::max(uv_a, uv_b);
    std::vector<Entity> out;
    for (const auto [e, mr] : world.registry().view<const MeshRendererComponent>().each()) {
        if (!scene::is_visible_in_hierarchy(world, e)) {
            continue;
        }
        const auto* mesh = cache.mesh(mr.mesh);
        if (mesh == nullptr) {
            continue;
        }
        const Vec3 c    = gameplay::transform_aabb(world.world_matrix(e), mesh->bounds).center();
        const Vec4 clip = view_proj * Vec4(c, 1.0f);
        if (clip.w <= 1e-6f) {
            continue; // behind the camera
        }
        const Vec2 uv = (Vec2(clip) / clip.w) * 0.5f + 0.5f; // matches make_pick_ray (Y already flipped)
        if (uv.x >= lo.x && uv.x <= hi.x && uv.y >= lo.y && uv.y <= hi.y) {
            out.push_back(e);
        }
    }
    return out;
}

std::optional<f32> ray_plane(const Ray& ray, const Vec3& normal, f32 d) {
    const f32 denom = glm::dot(normal, ray.direction);
    if (std::abs(denom) < 1e-8f) {
        return std::nullopt;
    }
    const f32 t = (d - glm::dot(normal, ray.origin)) / denom;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

u32 pick_id_of(Entity e) { return e == kNullEntity ? 0u : static_cast<u32>(entt::to_entity(e)) + 1u; }

Entity entity_from_pick_id(const World& world, u32 user_id) {
    if (user_id == 0) {
        return kNullEntity;
    }
    const auto index = static_cast<entt::id_type>(user_id - 1u);
    for (const Entity e : world.registry().view<const MeshRendererComponent>()) {
        if (static_cast<entt::id_type>(entt::to_entity(e)) == index) {
            return e;
        }
    }
    return kNullEntity;
}

// ---- GpuPickTracker ---------------------------------------------------------------------------
GpuPickTracker::GpuPickTracker(u32 timeout_frames, u32 unsupported_after)
    : timeout_(timeout_frames), unsupported_after_(std::max(unsupported_after, 1u)) {}

GpuPickTracker::Route GpuPickTracker::request(u64 frame, u32 tag) {
    if (pending_) {
        // Abandon the in-flight request: its answer may still arrive and must not be taken for
        // this click's.
        pending_      = false;
        ignore_until_ = frame + timeout_ + 1;
    }
    if (!gpu_available() || frame < ignore_until_) {
        // Resolve on the CPU now; a probe request_pick() by the caller may answer later.
        ignore_until_ = std::max(ignore_until_, frame + timeout_ + 1);
        return Route::Cpu;
    }
    pending_       = true;
    request_frame_ = frame;
    tag_           = tag;
    return Route::Gpu;
}

std::optional<GpuPickTracker::Outcome> GpuPickTracker::update(u64 frame, std::optional<u32> gpu_result) {
    if (!pending_) {
        if (gpu_result) {
            timeouts_ = 0; // stale or probe answer: discarded, but the GPU path works
        }
        return std::nullopt;
    }
    if (gpu_result) {
        pending_  = false;
        timeouts_ = 0;
        ++gpu_results_;
        return Outcome{ Source::Gpu, *gpu_result, tag_ };
    }
    if (frame > request_frame_ + timeout_) {
        pending_      = false;
        ignore_until_ = frame + timeout_ + 1;
        ++timeouts_;
        ++fallbacks_;
        return Outcome{ Source::CpuFallback, 0u, tag_ };
    }
    return std::nullopt;
}

void GpuPickTracker::reset() noexcept {
    pending_      = false;
    ignore_until_ = 0;
    timeouts_     = 0;
}

} // namespace aether::editor
