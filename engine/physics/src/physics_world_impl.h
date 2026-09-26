// physics_world_impl.h (private) — PhysicsWorld::Impl, shared by physics_world.cpp,
// physics_queries.cpp and physics_debug.cpp.
#pragma once

#include "contact_tracker.h"
#include "jolt_common.h"

#include "aether/physics/components.h"
#include "aether/physics/physics_world.h"
#include "aether/physics/runtime.h"
#include "aether/scene/components.h"
#include "aether/scene/world.h"

#include <map>
#include <memory>
#include <vector>

namespace aether::physics {

namespace detail {

struct Pose {
    Vec3 position{ 0.0f };
    Quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

// Rigid-transform view of an entity's world matrix.
struct WorldPose {
    Vec3 position{ 0.0f };
    Quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    Vec3 scale{ 1.0f };
};

// What the ECS looked like the last time physics and the ECS agreed on this entity's pose.
// Root entities are compared by local TRS (cheap); parented ones by composed world matrix.
struct SyncState {
    Transform local{};
    Entity    parent = kNullEntity;
    Mat4      world{ 1.0f };
};

struct BodyRecord {
    JPH::BodyID               id;
    MotionType                motion = MotionType::Static;
    bool                      sensor = false;
    bool                      kinematic_moving = false; // last MoveKinematic had non-zero motion
    Vec3                      scale{ 1.0f };            // world scale baked into the shape
    SyncState                 sync{};
    Pose                      prev{};
    Pose                      curr{};
    JPH::RefConst<JPH::Shape> base_shape; // undecorated primitive (debug drawing of hulls)
};

struct CharacterRecord {
    JPH::Ref<JPH::CharacterVirtual> character;
    Vec3                            synced_position{ 0.0f };
    Pose                            prev{};
    Pose                            curr{};
    f32                             radius = 0.3f;
    f32                             half_height = 0.6f;
    bool                            allow_sliding = true;
};

enum class ConstraintKind : u8 { Fixed = 0, Point, Hinge, Distance };

struct ConstraintRecord {
    Entity                   owner = kNullEntity;
    ConstraintKind           kind = ConstraintKind::Fixed;
    Entity                   target = kNullEntity;
    JPH::Ref<JPH::Constraint> constraint; // null while waiting for bodies
    JPH::BodyID              body1;
    JPH::BodyID              body2;       // invalid = world
    Vec3                     anchor1_local{ 0.0f }; // body1 space (debug drawing)
    Vec3                     anchor2_local{ 0.0f }; // body2 space, or world space if body2 invalid
};

struct PreservedVelocity {
    Vec3 linear{ 0.0f };
    Vec3 angular{ 0.0f };
};

class CharacterListener final : public JPH::CharacterContactListener {
public:
    explicit CharacterListener(const entt::storage<CharacterRecord>* records) : records_(records) {}

    // Stops idle characters from sliding down walkable slopes (Jolt sample behaviour).
    void OnContactSolve(const JPH::CharacterVirtual* character, const JPH::BodyID& body2,
                        const JPH::SubShapeID& sub_shape2, JPH::RVec3Arg contact_position,
                        JPH::Vec3Arg contact_normal, JPH::Vec3Arg contact_velocity,
                        const JPH::PhysicsMaterial* contact_material,
                        JPH::Vec3Arg character_velocity, JPH::Vec3& new_velocity) override;

private:
    const entt::storage<CharacterRecord>* records_;
};

// Math helpers (physics_world.cpp).
[[nodiscard]] WorldPose decompose(const Mat4& m);
[[nodiscard]] Mat4      compose(const Vec3& position, const Quat& rotation, const Vec3& scale);

} // namespace detail

struct PhysicsWorld::Impl {
    Impl(World& world, const PhysicsWorldSettings& settings);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // ---- ECS signal handlers ----
    void on_rigid_changed(entt::registry&, Entity e) { invalidate_body(e, true); }
    void on_rigid_removed(entt::registry&, Entity e) { invalidate_body(e, false); }
    void on_character_changed(entt::registry&, Entity e) { invalidate_character(e); }
    template <detail::ConstraintKind Kind>
    void on_constraint_changed(entt::registry&, Entity e) { erase_constraint(e, Kind); }

    // ---- lifecycle ----
    void enqueue(Entity e);
    void invalidate_body(Entity e, bool preserve_velocity);
    void invalidate_character(Entity e);
    void destroy_body(Entity e, bool preserve_velocity);
    void destroy_character(Entity e);
    void create_pending();
    void create_body(Entity e, std::vector<JPH::BodyID>& activate,
                     std::vector<JPH::BodyID>& dont_activate);
    void create_character(Entity e);

    // ---- constraints ----
    [[nodiscard]] static u64 constraint_key(Entity e, detail::ConstraintKind kind);
    void erase_constraint(Entity e, detail::ConstraintKind kind);
    void detach_constraints_of(const JPH::BodyID& body);
    void create_constraints();
    template <typename Component, detail::ConstraintKind Kind>
    void create_constraints_of();

    // ---- transforms ----
    [[nodiscard]] Entity    parent_of(Entity e) const;
    [[nodiscard]] u32       depth_of(Entity e) const;
    [[nodiscard]] Mat4      world_matrix_of(Entity e) const;
    [[nodiscard]] detail::WorldPose world_pose_of(Entity e) const;
    [[nodiscard]] bool      transform_edited(Entity e, const detail::SyncState& sync) const;
    [[nodiscard]] detail::SyncState capture_sync(Entity e) const;
    // Writes a simulated world pose into the entity's local transform; returns the new sync state.
    detail::SyncState write_back(Entity e, const detail::Pose& pose, bool write_rotation);

    // ---- stepping ----
    void sync_internal(f32 dt); // dt > 0: kinematic bodies MoveKinematic; dt == 0: teleport
    void push_ecs_edits(f32 dt, std::vector<Entity>& rebuilds);
    void teleport(Entity e, detail::BodyRecord& rec, std::vector<Entity>& rebuilds);
    void update_characters(f32 dt);
    void pull_results();

    [[nodiscard]] JPH::BodyInterface& bodies() const { return system->GetBodyInterface(); }
    [[nodiscard]] const detail::BodyRecord* body_record(Entity e) const {
        return bodies_.contains(e) ? &bodies_.get(e) : nullptr;
    }
    [[nodiscard]] detail::BodyRecord* body_record(Entity e) {
        return bodies_.contains(e) ? &bodies_.get(e) : nullptr;
    }
    // Calls fn(BodyInterface&, BodyID) if `e` has a Dynamic body.
    template <typename Fn>
    void with_dynamic(Entity e, Fn&& fn) const {
        if (const detail::BodyRecord* rec = body_record(e); rec && rec->motion == MotionType::Dynamic)
            fn(bodies(), rec->id);
    }

    // Declaration order == construction order: the runtime reference must outlive every Jolt
    // object below it.
    RuntimeRef           runtime;
    World&               world;
    entt::registry&      reg;
    PhysicsWorldSettings settings;

    detail::BroadPhaseLayerMapper      bp_mapper;
    detail::ObjectVsBroadPhaseFilter   obj_vs_bp_filter;
    detail::ObjectLayerPairFilterImpl  pair_filter;
    detail::ContactRecorder            recorder;
    detail::ContactTracker             tracker;

    std::unique_ptr<JPH::TempAllocator> temp_allocator;
    std::unique_ptr<JPH::JobSystem>     job_system;
    std::unique_ptr<JPH::PhysicsSystem> system;
    detail::CharacterListener                character_listener;

    entt::storage<detail::BodyRecord>        bodies_;
    entt::sparse_set                         moving_;      // entities with Kinematic/Dynamic bodies
    entt::storage<detail::CharacterRecord>   characters_;
    entt::storage<detail::PreservedVelocity> preserved_;   // velocities carried across rebuilds
    std::map<u64, detail::ConstraintRecord>  constraints_; // ordered => deterministic iteration
    std::vector<Entity>                      pending_;
    entt::sparse_set                         pending_set_;

    std::vector<ContactEvent>         events;
    std::vector<detail::RawContact>   raw_contacts;
    std::vector<detail::DebugContact> debug_contacts;
    std::vector<entt::connection>     connections;

    u64  step_count = 0;
    bool stepping = false;
    bool broadphase_optimized = false;
    u32  reported_update_errors = 0; // EPhysicsUpdateError bits already logged
};

} // namespace aether::physics
