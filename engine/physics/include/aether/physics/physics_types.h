// aether/physics/physics_types.h — shared physics vocabulary: motion types, collision layers,
// query hits/filters, contact events and debug lines.
//
// Everything here is plain data (no Jolt types; blueprint §4.1 keeps third-party types private).
// Units: meters, kilograms, seconds, radians unless a name says otherwise (`*_deg`).
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <array>

namespace aether::physics {

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------
enum class MotionType : u8 {
    Static = 0, // never moves (ground, level geometry); may be teleported by editing its transform
    Kinematic,  // moved by gameplay through its TransformComponent; pushes dynamic bodies
    Dynamic,    // fully simulated; physics writes its TransformComponent
};

// Degrees of freedom that can be locked on a body (bitmask, see RigidBodyComponent::locked_dofs).
// DOFs are world-space axes (Jolt semantics).
namespace dof {
inline constexpr u8 None         = 0;
inline constexpr u8 TranslationX = 1u << 0;
inline constexpr u8 TranslationY = 1u << 1;
inline constexpr u8 TranslationZ = 1u << 2;
inline constexpr u8 RotationX    = 1u << 3;
inline constexpr u8 RotationY    = 1u << 4;
inline constexpr u8 RotationZ    = 1u << 5;
inline constexpr u8 AllTranslation = TranslationX | TranslationY | TranslationZ;
inline constexpr u8 AllRotation    = RotationX | RotationY | RotationZ;
// Lock everything except X/Y translation and Z rotation (2D physics in the XY plane).
inline constexpr u8 Plane2D = TranslationZ | RotationX | RotationY;
} // namespace dof

// ---------------------------------------------------------------------------
// Collision layers
// ---------------------------------------------------------------------------
// Gameplay-facing collision layers (0..15). Internally every layer is split into a
// static and a moving object layer, which map onto Jolt's two broadphase trees
// (Static / Moving), so static-vs-static pairs are never even considered.
inline constexpr u32 kMaxCollisionLayers = 16;
using CollisionLayer = u8;

// Symmetric 16x16 "which layers collide" matrix. Default: everything collides.
class CollisionLayerMatrix {
public:
    constexpr CollisionLayerMatrix() { masks_.fill(0xFFFFu); }

    // Enable/disable collision between layers a and b (symmetric).
    constexpr void set_collides(CollisionLayer a, CollisionLayer b, bool collides) {
        if (a >= kMaxCollisionLayers || b >= kMaxCollisionLayers)
            return;
        const u16 bit_a = static_cast<u16>(1u << a);
        const u16 bit_b = static_cast<u16>(1u << b);
        if (collides) {
            masks_[a] = static_cast<u16>(masks_[a] | bit_b);
            masks_[b] = static_cast<u16>(masks_[b] | bit_a);
        } else {
            masks_[a] = static_cast<u16>(masks_[a] & ~bit_b);
            masks_[b] = static_cast<u16>(masks_[b] & ~bit_a);
        }
    }

    [[nodiscard]] constexpr bool collides(CollisionLayer a, CollisionLayer b) const {
        if (a >= kMaxCollisionLayers || b >= kMaxCollisionLayers)
            return false;
        return (masks_[a] & (1u << b)) != 0;
    }

    // Bitmask of the layers that `layer` collides with.
    [[nodiscard]] constexpr u16 mask(CollisionLayer layer) const {
        return layer < kMaxCollisionLayers ? masks_[layer] : u16{ 0 };
    }

private:
    std::array<u16, kMaxCollisionLayers> masks_{};
};

// ---------------------------------------------------------------------------
// Scene queries
// ---------------------------------------------------------------------------
struct QueryFilter {
    u16    layer_mask      = 0xFFFFu;     // bit i set => bodies on collision layer i are tested
    Entity ignore_entity   = kNullEntity; // e.g. the querying character itself
    bool   include_static  = true;        // static bodies
    bool   include_moving  = true;        // kinematic + dynamic bodies (and character bodies)
    bool   include_sensors = false;       // trigger volumes are skipped unless requested
};

// Result of a raycast, sweep or overlap query.
struct QueryHit {
    Entity entity = kNullEntity;
    Vec3   point{ 0.0f };           // world-space contact point on the hit body's surface
    Vec3   normal{ 0.0f };          // world-space surface normal of the hit body at `point`,
                                    // pointing towards the query (unit length)
    f32    fraction = 0.0f;         // [0,1] along the cast; hit position = origin + fraction*dir*max_distance
                                    // (0 for overlaps and for sweeps that start penetrating)
    f32    distance = 0.0f;         // fraction * max_distance
    f32    penetration_depth = 0.0f; // overlaps / initially-penetrating sweeps only
};

// ---------------------------------------------------------------------------
// Contact & trigger events
// ---------------------------------------------------------------------------
enum class ContactEventType : u8 {
    Begin = 0, // first step in which the two bodies touch (or a body entered a trigger)
    Persist,   // the pair is still touching this step (one event per pair per step)
    End,       // the pair stopped touching (or left the trigger). Also emitted when one of the
               // bodies is destroyed or falls asleep (Jolt only tracks contacts of active bodies).
};

// Body-pair level contact event, aggregated from Jolt's sub-shape contacts and delivered in a
// deterministic order on the main thread after each step.
struct ContactEvent {
    ContactEventType type = ContactEventType::Begin;
    bool   is_trigger = false;  // at least one body is a sensor; then `a` is the sensor
    Entity a = kNullEntity;     // NOTE: for End events either entity may already be destroyed
    Entity b = kNullEntity;
    Vec3   point{ 0.0f };       // world-space average contact point (Begin/Persist; zero for End)
    Vec3   normal{ 0.0f };      // world-space contact normal pointing from a towards b
    f32    penetration_depth = 0.0f;
    f32    approach_speed = 0.0f; // relative normal speed at the contact (> 0: approaching);
                                  // useful for impact sounds / damage
};

// ---------------------------------------------------------------------------
// Debug visualisation
// ---------------------------------------------------------------------------
// A world-space debug line. `rgba` is packed RGBA8 in memory order (R in the lowest byte,
// i.e. VK_FORMAT_R8G8B8A8_UNORM), the same packing as renderer::RenderLine colors; the gameplay
// layer copies these into RenderScene::debug_lines (physics never depends on the renderer).
struct DebugLine {
    Vec3 a{ 0.0f };
    Vec3 b{ 0.0f };
    u32  rgba = 0xFFFFFFFFu;
};

[[nodiscard]] constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

// What build_debug_lines() emits (bitmask).
enum class DebugDrawFlags : u32 {
    None        = 0,
    Shapes      = 1u << 0, // collider wireframes, colored by motion type / sleep / sensor state
    Contacts    = 1u << 1, // contact points + normals of the last step
    Characters  = 1u << 2, // character capsules + ground normal
    Constraints = 1u << 3, // constraint anchors / connections
    Velocities  = 1u << 4, // linear velocity of active dynamic bodies
    Default     = Shapes | Contacts | Characters | Constraints,
    All         = 0xFFFFFFFFu,
};

[[nodiscard]] constexpr DebugDrawFlags operator|(DebugDrawFlags a, DebugDrawFlags b) {
    return static_cast<DebugDrawFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr DebugDrawFlags operator&(DebugDrawFlags a, DebugDrawFlags b) {
    return static_cast<DebugDrawFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_flag(DebugDrawFlags set, DebugDrawFlags flag) {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0;
}

} // namespace aether::physics
