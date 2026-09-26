// aether/physics/components.h — ECS components owned by aether.physics.
//
// Authoring model (Unity/Unreal-like):
//   * An entity with a ColliderComponent (+ the TransformComponent every entity has) gets a
//     physics body. Without a RigidBodyComponent the body is Static; add a RigidBodyComponent to
//     make it Kinematic/Dynamic or to set material/sensor/layer properties.
//   * CharacterControllerComponent makes the entity a Jolt CharacterVirtual (capsule; the entity
//     origin is at the character's FEET). Do not combine it with a ColliderComponent.
//   * Constraint components join the entity's body to `target`'s body (or to the world when
//     `target` is kNullEntity). Anchors/axes are in the owning entity's local space and are
//     resolved to world space when the constraint is created.
//
// Change tracking: PhysicsWorld listens to EnTT construct/update/destroy signals, so
// `world.add<T>()` (which replaces an existing component) and `world.remove<T>()` take effect on
// the next PhysicsWorld::sync()/step(). Editing a field IN PLACE (world.get<T>(e).mass = 3) is not
// observable - call PhysicsWorld::rebuild_body(e) afterwards. The per-frame input fields of
// CharacterControllerComponent are the exception: they are read every step.
//
// Thread affinity: components are main-thread data like every ECS component.
#pragma once

#include "aether/core/math.h"
#include "aether/core/reflect.h"
#include "aether/core/types.h"
#include "aether/physics/physics_types.h"
#include "aether/scene/entity.h"

#include <utility>
#include <vector>

namespace aether::physics {

// ---------------------------------------------------------------------------
// Rigid body
// ---------------------------------------------------------------------------
struct RigidBodyComponent {
    MotionType motion_type = MotionType::Dynamic;

    // Mass in kg. <= 0: computed from the collider volume at 1000 kg/m^3 (water); > 0: the
    // collider's inertia is scaled to this mass.
    f32 mass = 0.0f;

    f32 friction        = 0.5f;  // combined as sqrt(f1*f2)
    f32 restitution     = 0.0f;  // combined as max(r1, r2); 0 = inelastic, 1 = perfectly elastic
    f32 linear_damping  = 0.05f; // dv/dt = -c*v
    f32 angular_damping = 0.05f;
    f32 gravity_factor  = 1.0f;

    // Trigger volume: reports events, no collision response. A Static sensor (cheapest) only sees
    // awake Dynamic/Kinematic bodies - a body falling asleep inside it produces an End event.
    // Make the sensor Kinematic if it must keep detecting sleeping bodies.
    bool is_sensor             = false;
    bool continuous_collision  = false; // swept (linear-cast) CCD for fast, small dynamic bodies
    bool allow_sleeping        = true;

    CollisionLayer collision_layer = 0; // 0..kMaxCollisionLayers-1, see PhysicsWorld layer matrix
    u8             locked_dofs     = dof::None; // bitmask of physics::dof::* (Dynamic bodies)

    // Applied once, when the body is first created for this entity (not on rebuilds).
    Vec3 initial_linear_velocity{ 0.0f };  // world space, m/s
    Vec3 initial_angular_velocity{ 0.0f }; // world space, rad/s

    AE_REFLECT(RigidBodyComponent,
        AE_FIELD(motion_type), AE_FIELD(mass), AE_FIELD(friction), AE_FIELD(restitution),
        AE_FIELD(linear_damping), AE_FIELD(angular_damping), AE_FIELD(gravity_factor),
        AE_FIELD(is_sensor), AE_FIELD(continuous_collision), AE_FIELD(allow_sleeping),
        AE_FIELD(collision_layer), AE_FIELD(locked_dofs))
};

// ---------------------------------------------------------------------------
// Collider
// ---------------------------------------------------------------------------
enum class ColliderShape : u8 {
    Box = 0,      // half_extents
    Sphere,       // radius
    Capsule,      // radius + half_height (half length of the cylindrical part), along local Y
    Cylinder,     // radius + half_height, along local Y
    ConvexHull,   // points (collider-local space)
    TriangleMesh, // vertices + indices (triangle list); STATIC bodies only
};

struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;

    Vec3 half_extents{ 0.5f }; // Box
    f32  radius      = 0.5f;   // Sphere / Capsule / Cylinder
    f32  half_height = 0.5f;   // Capsule (cylindrical part) / Cylinder

    std::vector<Vec3> points;   // ConvexHull
    std::vector<Vec3> vertices; // TriangleMesh
    std::vector<u32>  indices;  // TriangleMesh, 3 per triangle (counter-clockwise front faces)

    // Placement of the shape relative to the entity origin (entity local space). The entity's
    // world scale is applied on top (non-uniform scale on rotated shapes is approximated).
    Vec3 local_offset{ 0.0f };
    Quat local_rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    // --- convenience factories -------------------------------------------------------------
    [[nodiscard]] static ColliderComponent box(const Vec3& half_extents) {
        ColliderComponent c;
        c.shape = ColliderShape::Box;
        c.half_extents = half_extents;
        return c;
    }
    [[nodiscard]] static ColliderComponent sphere(f32 radius) {
        ColliderComponent c;
        c.shape = ColliderShape::Sphere;
        c.radius = radius;
        return c;
    }
    [[nodiscard]] static ColliderComponent capsule(f32 half_height, f32 radius) {
        ColliderComponent c;
        c.shape = ColliderShape::Capsule;
        c.half_height = half_height;
        c.radius = radius;
        return c;
    }
    [[nodiscard]] static ColliderComponent cylinder(f32 half_height, f32 radius) {
        ColliderComponent c;
        c.shape = ColliderShape::Cylinder;
        c.half_height = half_height;
        c.radius = radius;
        return c;
    }
    [[nodiscard]] static ColliderComponent convex_hull(std::vector<Vec3> hull_points) {
        ColliderComponent c;
        c.shape = ColliderShape::ConvexHull;
        c.points = std::move(hull_points);
        return c;
    }
    [[nodiscard]] static ColliderComponent triangle_mesh(std::vector<Vec3> mesh_vertices,
                                                         std::vector<u32>  mesh_indices) {
        ColliderComponent c;
        c.shape = ColliderShape::TriangleMesh;
        c.vertices = std::move(mesh_vertices);
        c.indices = std::move(mesh_indices);
        return c;
    }

    AE_REFLECT(ColliderComponent,
        AE_FIELD(shape), AE_FIELD(half_extents), AE_FIELD(radius), AE_FIELD(half_height),
        AE_FIELD(local_offset), AE_FIELD(local_rotation))
};

// ---------------------------------------------------------------------------
// Character controller (Jolt CharacterVirtual)
// ---------------------------------------------------------------------------
enum class GroundState : u8 {
    OnGround = 0,  // standing on walkable ground
    OnSteepGround, // touching ground that is too steep to stand on (will slide)
    NotSupported,  // touching something, but it doesn't support the character
    InAir,         // not touching anything below
};

struct CharacterControllerComponent {
    // ---- settings (rebuild on change: world.add<>() again or PhysicsWorld::rebuild_body) ----
    f32 radius        = 0.3f;  // capsule radius
    f32 half_height   = 0.6f;  // half length of the capsule's cylinder; total height = 2*(half_height+radius)
    f32 max_slope_deg = 45.0f; // steeper surfaces behave like walls
    f32 step_up_height = 0.35f;          // max stair height walked up automatically (0 = off)
    f32 stick_to_floor_distance = 0.5f;  // snap down this far when walking off small ledges (0 = off)
    f32 mass         = 70.0f;  // kg, used to push down on dynamic bodies the character stands on
    f32 max_strength = 100.0f; // N, max force with which the character pushes dynamic bodies
    CollisionLayer collision_layer = 0;

    // ---- per-step input (read every fixed step; not change-tracked) ----
    Vec3 desired_velocity{ 0.0f }; // world-space horizontal velocity (the Y component is ignored)
    f32  jump_speed = 5.0f;        // upward speed applied when a jump is performed
    bool jump_requested = false;   // one-shot: consumed (reset to false) by the next step
    bool apply_gravity = true;

    // ---- outputs (written by PhysicsWorld after every step) ----
    bool        on_ground = false; // ground_state == OnGround
    GroundState ground_state = GroundState::InAir;
    Vec3        ground_normal{ 0.0f, 1.0f, 0.0f };
    Vec3        velocity{ 0.0f };  // linear velocity after the step (world space, m/s)
    Entity      ground_entity = kNullEntity;

    AE_REFLECT(CharacterControllerComponent,
        AE_FIELD(radius), AE_FIELD(half_height), AE_FIELD(max_slope_deg), AE_FIELD(step_up_height),
        AE_FIELD(stick_to_floor_distance), AE_FIELD(mass), AE_FIELD(max_strength),
        AE_FIELD(collision_layer))
};

// ---------------------------------------------------------------------------
// Constraints (joints). The owning entity is body 1; `target` is body 2 (kNullEntity = world).
// At least one of the two bodies must be Dynamic for the constraint to have any effect.
// ---------------------------------------------------------------------------

// Welds the two bodies together in their relative pose at creation time.
struct FixedConstraintComponent {
    Entity target = kNullEntity;

    AE_REFLECT(FixedConstraintComponent, AE_FIELD(target))
};

// Ball-and-socket: the anchor point is shared by both bodies, rotation is free.
struct PointConstraintComponent {
    Entity target = kNullEntity;
    Vec3   local_anchor{ 0.0f }; // in the owning entity's local space

    AE_REFLECT(PointConstraintComponent, AE_FIELD(target), AE_FIELD(local_anchor))
};

// Hinge: shared anchor point + a single rotation axis, with optional angle limits.
struct HingeConstraintComponent {
    Entity target = kNullEntity;
    Vec3   local_anchor{ 0.0f };               // owning entity local space
    Vec3   local_axis{ 0.0f, 1.0f, 0.0f };     // hinge axis, owning entity local space
    bool   limits_enabled = false;
    f32    min_angle_deg = -180.0f;            // [-180, 0]
    f32    max_angle_deg = 180.0f;             // [0, 180]
    f32    max_friction_torque = 0.0f;         // N*m, resists rotation (0 = frictionless)

    AE_REFLECT(HingeConstraintComponent,
        AE_FIELD(target), AE_FIELD(local_anchor), AE_FIELD(local_axis), AE_FIELD(limits_enabled),
        AE_FIELD(min_angle_deg), AE_FIELD(max_angle_deg), AE_FIELD(max_friction_torque))
};

// Keeps the distance between two anchor points within [min_distance, max_distance].
struct DistanceConstraintComponent {
    Entity target = kNullEntity;
    Vec3   local_anchor{ 0.0f };        // owning entity local space
    Vec3   target_local_anchor{ 0.0f }; // target entity local space (world space if target is null)
    f32    min_distance = -1.0f;        // < 0: use the distance at creation time
    f32    max_distance = -1.0f;        // < 0: use the distance at creation time
    f32    spring_frequency = 0.0f;     // Hz; 0 = rigid limits, > 0 = soft (spring) limits
    f32    spring_damping = 0.0f;       // damping ratio for the soft limits

    AE_REFLECT(DistanceConstraintComponent,
        AE_FIELD(target), AE_FIELD(local_anchor), AE_FIELD(target_local_anchor),
        AE_FIELD(min_distance), AE_FIELD(max_distance), AE_FIELD(spring_frequency),
        AE_FIELD(spring_damping))
};

} // namespace aether::physics
