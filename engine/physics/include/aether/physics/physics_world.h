// aether/physics/physics_world.h — the ECS-driven physics world (Jolt PhysicsSystem wrapper).
//
// One PhysicsWorld mirrors one aether::World:
//   * Body lifecycle follows the ECS. Bodies/characters/constraints are created on the first
//     sync()/step() after their components are added, rebuilt when a component is replaced
//     (world.add<T> on an existing component) and destroyed immediately when the component is
//     removed or the entity destroyed (EnTT on_destroy signals).
//   * ECS -> Jolt: Kinematic bodies follow their TransformComponent through MoveKinematic every
//     step. Static/Dynamic bodies and characters are TELEPORTED when their world transform was
//     edited (by gameplay/editor, or because an ancestor moved) since the last step.
//   * Jolt -> ECS: after each step Dynamic bodies and characters write their pose back into
//     TransformComponent::local (correctly converted for parented entities; scale untouched),
//     update TransformComponent::world and set `dirty`.
//   * Rendering interpolation: previous + current poses are kept per body;
//     interpolated_world_matrix(e, alpha) blends them (alpha = accumulator / fixed_dt).
//
// Determinism: given the same sequence of API calls/ECS mutations, two PhysicsWorlds produce
// bit-identical results (Jolt is built with JPH_CROSS_PLATFORM_DETERMINISTIC and /fp:precise).
// Contact events are sorted before delivery so their order is deterministic as well.
//
// Thread affinity: MAIN THREAD ONLY, except the query functions (raycast/sweep/overlap), which
// may run concurrently with each other from any thread but never concurrently with
// sync()/step() or with ECS changes to physics components.
//
// Lifetime: the PhysicsWorld must be destroyed before the World it observes.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_types.h"
#include "aether/scene/entity.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace aether {
class World;
}

namespace aether::physics {

// Which Jolt job system runs the multi-threaded parts of a step.
enum class PhysicsJobBackend : u8 {
    // EngineJobSystem if aether::JobSystem is running (worker_count() > 0), else JoltThreadPool.
    Auto = 0,
    // Adapter over aether::JobSystem (JPH::JobSystemWithBarrier). No extra threads; the calling
    // thread helps execute physics jobs while it waits, so a busy engine pool can't deadlock it.
    EngineJobSystem,
    // Jolt's own JPH::JobSystemThreadPool with `worker_threads` threads. Trade-off: those threads
    // compete with aether::JobSystem's workers for the same cores (oversubscription) - fine for
    // tools/tests, not recommended in the shipping game loop.
    JoltThreadPool,
    // Everything runs on the calling thread (debugging, tiny scenes, deterministic tooling).
    SingleThreaded,
};

struct PhysicsWorldSettings {
    Vec3 gravity{ 0.0f, -9.81f, 0.0f };

    u32 max_bodies              = 65536; // hard capacity (bodies + character inner bodies)
    u32 max_body_pairs          = 65536; // broadphase pair buffer
    u32 max_contact_constraints = 10240; // contact manifold constraints per step
    u32 num_body_mutexes        = 0;     // 0 = Jolt default

    // Collision sub-steps per fixed step (Jolt "collision steps"). Use >1 for large fixed dts
    // (Jolt recommends one collision step per 1/60 s).
    u32 collision_steps = 1;

    // Resting contacts are allowed to penetrate this far before being pushed apart (Jolt
    // PhysicsSettings::mPenetrationSlop, whose own default is 0.02 m). Smaller = objects rest
    // closer to their true height; too small makes large stacks jittery.
    f32 penetration_slop = 0.005f;

    u32 temp_allocator_bytes = 16u * 1024u * 1024u; // per-step scratch (falls back to malloc)

    PhysicsJobBackend job_backend = PhysicsJobBackend::Auto;
    i32               worker_threads = -1; // JoltThreadPool only; -1 = max(1, hw - 1)
    u32               max_jobs = 2048;     // JoltThreadPool / EngineJobSystem job pool capacity
    u32               max_barriers = 8;

    CollisionLayerMatrix layers{}; // which of the 16 collision layers collide

    // Detect edits to STATIC bodies' transforms every step (editor, movable level pieces).
    // Costs one TransformComponent compare per static body per step; games whose static
    // geometry never moves can turn it off.
    bool track_static_transforms = true;

    bool emit_persist_events = true;  // ContactEventType::Persist once per touching pair per step
    u32  max_buffered_events = 65536; // events beyond this (undrained) are dropped with a warning
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(World& world, const PhysicsWorldSettings& settings = {});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    [[nodiscard]] World&                      world() const;
    [[nodiscard]] const PhysicsWorldSettings& settings() const;

    // ---- simulation -------------------------------------------------------------------------
    // Creates pending bodies/characters/constraints and pushes ECS transform edits to Jolt.
    // step() calls this first; call it directly to make freshly added bodies visible to queries
    // without advancing time.
    void sync();

    // Advances the simulation by exactly `dt` seconds (one fixed step): sync, character update,
    // Jolt update, write-back to the ECS and contact-event aggregation.
    void step(f32 dt);

    [[nodiscard]] u64 step_count() const;

    void               set_gravity(const Vec3& gravity);
    [[nodiscard]] Vec3 gravity() const;

    // Collision layer matrix (takes effect for new contacts/queries immediately).
    void               set_layer_collision(CollisionLayer a, CollisionLayer b, bool collides);
    [[nodiscard]] bool layers_collide(CollisionLayer a, CollisionLayer b) const;

    // Rebuilds broadphase trees for query/collision performance. Call once after bulk-loading a
    // level (never per frame). step() does this automatically on its first call.
    void optimize_broadphase();

    // ---- bodies -----------------------------------------------------------------------------
    [[nodiscard]] bool  has_body(Entity e) const;      // rigid body exists (not characters)
    [[nodiscard]] bool  has_character(Entity e) const;
    [[nodiscard]] usize body_count() const;            // rigid bodies (excl. character bodies)
    [[nodiscard]] usize character_count() const;
    [[nodiscard]] usize constraint_count() const;      // live Jolt constraints
    [[nodiscard]] usize active_body_count() const;     // awake Dynamic/Kinematic bodies (Jolt)
    [[nodiscard]] bool  is_awake(Entity e) const;
    [[nodiscard]] std::optional<MotionType> motion_type(Entity e) const;

    void wake_up(Entity e);

    // Re-reads the entity's physics components (after in-place field edits) and rebuilds its
    // body/character/constraints on the next sync. Dynamic velocities are preserved.
    void rebuild_body(Entity e);

    // Current simulated pose of the body/character (world space; entity origin).
    [[nodiscard]] std::optional<Transform> body_pose(Entity e) const;

    // ---- velocities & forces (Dynamic bodies; forces/torques accumulate until the next step) --
    [[nodiscard]] Vec3 linear_velocity(Entity e) const;  // characters: last velocity
    [[nodiscard]] Vec3 angular_velocity(Entity e) const;
    void set_linear_velocity(Entity e, const Vec3& v);   // also valid on characters
    void set_angular_velocity(Entity e, const Vec3& w);

    void add_force(Entity e, const Vec3& force);                                   // N, at COM
    void add_force_at_point(Entity e, const Vec3& force, const Vec3& world_point); // N
    void add_torque(Entity e, const Vec3& torque);                                 // N*m
    void add_impulse(Entity e, const Vec3& impulse);                               // N*s, at COM
    void add_impulse_at_point(Entity e, const Vec3& impulse, const Vec3& world_point);
    void add_angular_impulse(Entity e, const Vec3& angular_impulse);               // N*m*s

    // ---- render interpolation ---------------------------------------------------------------
    // Dynamic bodies and characters: blend of the pose before and after the last step
    // (alpha in [0,1]; 1 = latest simulated pose), with the entity's world scale applied.
    // Other entities: their current ECS world matrix (they are driven by the ECS already).
    [[nodiscard]] Mat4 interpolated_world_matrix(Entity e, f32 alpha) const;

    // ---- scene queries (see thread-affinity note above) ---------------------------------------
    // `direction` need not be normalised (must be non-zero); casts travel `max_distance` meters.
    [[nodiscard]] std::optional<QueryHit> raycast(const Vec3& origin, const Vec3& direction,
                                                  f32 max_distance,
                                                  const QueryFilter& filter = {}) const;
    // All hits along the ray (at most one per entity), sorted by increasing fraction.
    [[nodiscard]] std::vector<QueryHit> raycast_all(const Vec3& origin, const Vec3& direction,
                                                    f32 max_distance,
                                                    const QueryFilter& filter = {}) const;

    // Closest hit of a shape swept from `origin` along `direction`.
    [[nodiscard]] std::optional<QueryHit> sweep_sphere(f32 radius, const Vec3& origin,
                                                       const Vec3& direction, f32 max_distance,
                                                       const QueryFilter& filter = {}) const;
    [[nodiscard]] std::optional<QueryHit> sweep_box(const Vec3& half_extents, const Quat& rotation,
                                                    const Vec3& origin, const Vec3& direction,
                                                    f32 max_distance,
                                                    const QueryFilter& filter = {}) const;
    // Capsule along its local Y axis (half_height = half length of the cylindrical part).
    [[nodiscard]] std::optional<QueryHit> sweep_capsule(f32 half_height, f32 radius,
                                                        const Quat& rotation, const Vec3& origin,
                                                        const Vec3& direction, f32 max_distance,
                                                        const QueryFilter& filter = {}) const;

    // Every entity overlapping the shape (one hit per entity: the deepest), sorted by entity.
    [[nodiscard]] std::vector<QueryHit> overlap_sphere(const Vec3& center, f32 radius,
                                                       const QueryFilter& filter = {}) const;
    [[nodiscard]] std::vector<QueryHit> overlap_box(const Vec3& center, const Vec3& half_extents,
                                                    const Quat& rotation,
                                                    const QueryFilter& filter = {}) const;

    // ---- contact & trigger events -----------------------------------------------------------
    // Events accumulated by every step since the last clear/drain, in deterministic order.
    [[nodiscard]] std::span<const ContactEvent> contact_events() const;
    void clear_contact_events();
    // Appends all buffered events to `out` and clears the buffer.
    void drain_contact_events(std::vector<ContactEvent>& out);

    // ---- debug visualisation ----------------------------------------------------------------
    // Appends world-space lines for the requested categories (current simulated poses).
    void build_debug_lines(std::vector<DebugLine>& out,
                           DebugDrawFlags flags = DebugDrawFlags::Default) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::physics
