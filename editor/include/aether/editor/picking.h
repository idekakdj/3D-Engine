// aether/editor/picking.h — viewport ray picking (CPU, against world-space bounds).
//
// make_pick_ray() unprojects a viewport position through the engine's projection convention
// (math.h perspective(): right-handed, depth 0..1, Y flipped for Vulkan), so uv (0,0) is the
// top-left of the viewport and (1,1) the bottom-right. pick_entity() tests every visible mesh
// renderer's world AABB (mesh bounds from the render cache) and returns the nearest hit.
//
// Main thread only (pick_entity reads the world and the cache).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <optional>

namespace aether {
class World;
}
namespace aether::gameplay {
class RenderResourceCache;
}

namespace aether::editor {

struct Ray {
    Vec3 origin{ 0.0f };
    Vec3 direction{ 0.0f, 0.0f, -1.0f }; // unit length
};

[[nodiscard]] Ray make_pick_ray(const Mat4& view, const Mat4& proj, const Vec2& viewport_uv);

// Distance along the ray to the box (0 if the origin is inside), or nullopt when missed.
[[nodiscard]] std::optional<f32> ray_aabb(const Ray& ray, const AABB& box);

struct PickHit {
    Entity entity   = kNullEntity;
    f32    distance = 0.0f;
};

[[nodiscard]] std::optional<PickHit> pick_entity(const World& world, gameplay::RenderResourceCache& cache,
                                                 const Ray& ray);

} // namespace aether::editor
