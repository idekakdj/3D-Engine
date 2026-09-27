// aether/editor/picking.h — viewport picking: CPU rays against world bounds, and the async GPU
// id-buffer pick (ADR-0009) with its CPU fallback.
//
// make_pick_ray() unprojects a viewport position through the engine's projection convention
// (math.h perspective(): right-handed, depth 0..1, Y flipped for Vulkan), so uv (0,0) is the
// top-left of the viewport and (1,1) the bottom-right. pick_entity() tests every visible mesh
// renderer's world AABB (mesh bounds from the render cache) and returns the nearest hit.
//
// GPU picking: Renderer::request_pick(pixel) asks for the RenderMeshInstance::user_id under a pixel
// of the next render; poll_pick() delivers it frames-in-flight later. The render bridge writes
// entity index + 1 (0 = nothing). GpuPickTracker is the GPU-free state machine deciding, per
// click, between the GPU answer and the CPU ray pick (see the class comment).
//
// Main thread only (pick_entity reads the world and the cache).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <optional>
#include <vector>

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

// Box selection: visible mesh-renderer entities whose world-bounds centre projects (through
// `view_proj`, engine convention) inside the viewport-uv rectangle spanned by a and b.
[[nodiscard]] std::vector<Entity> entities_in_rect(const World& world, gameplay::RenderResourceCache& cache,
                                                   const Mat4& view_proj, const Vec2& uv_a, const Vec2& uv_b);

// Distance along the ray to the plane dot(normal, x) = d, or nullopt when parallel / behind.
[[nodiscard]] std::optional<f32> ray_plane(const Ray& ray, const Vec3& normal, f32 d);

// GPU pick ids (render bridge convention: entity index + 1; 0 = nothing).
[[nodiscard]] u32    pick_id_of(Entity e);
// The live mesh-renderer entity whose index matches `user_id` (kNullEntity for 0 / stale ids).
[[nodiscard]] Entity entity_from_pick_id(const World& world, u32 user_id);

// Async GPU pick with CPU fallback.
//   request(): Route::Gpu -> the caller issues Renderer::request_pick() and waits for update() to
//              resolve it; Route::Cpu -> the caller resolves now with the CPU ray (GPU picking is
//              believed unsupported, or a previous GPU request is still in flight). On Route::Cpu
//              the caller should still call request_pick() as a probe: a result arriving while no
//              pick is pending is discarded but proves GPU picking works again.
//   update():  once per frame with Renderer::poll_pick(). Returns the outcome when the pending
//              pick completes (Gpu, with the id) or when `timeout_frames` frames passed without
//              an answer (CpuFallback: resolve with the ray captured at request time).
// After `unsupported_after` consecutive timeouts GPU picking is considered unsupported (clicks
// resolve immediately on the CPU); any GPU answer re-enables it. After a timeout or an abandoned
// request, answers arriving within `timeout_frames` frames are discarded: the API carries no
// request ids, so a late answer must never be attributed to a newer click.
class GpuPickTracker {
public:
    enum class Route : u8 { Gpu = 0, Cpu };
    enum class Source : u8 { Gpu = 0, CpuFallback };
    struct Outcome {
        Source source  = Source::Gpu;
        u32    user_id = 0; // valid for Source::Gpu
        u32    tag     = 0; // the request's tag
    };

    explicit GpuPickTracker(u32 timeout_frames = 4, u32 unsupported_after = 2);

    void  set_timeout_frames(u32 frames) noexcept { timeout_ = frames; }
    Route request(u64 frame, u32 tag = 0);
    std::optional<Outcome> update(u64 frame, std::optional<u32> gpu_result);

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] bool gpu_available() const noexcept { return timeouts_ < unsupported_after_; }
    [[nodiscard]] u32  consecutive_timeouts() const noexcept { return timeouts_; }
    [[nodiscard]] u32  gpu_results() const noexcept { return gpu_results_; }       // total answers used
    [[nodiscard]] u32  fallbacks() const noexcept { return fallbacks_; }           // total timeouts
    [[nodiscard]] u32  timeout_frames() const noexcept { return timeout_; }
    void               reset() noexcept;

private:
    u32  timeout_;
    u32  unsupported_after_;
    bool pending_       = false;
    u64  request_frame_ = 0;
    u32  tag_           = 0;
    u64  ignore_until_  = 0; // answers before this frame (with nothing pending) are stale
    u32  timeouts_      = 0;
    u32  gpu_results_   = 0;
    u32  fallbacks_     = 0;
};

} // namespace aether::editor
