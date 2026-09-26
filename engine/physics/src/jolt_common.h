// jolt_common.h (private) — the one place aether.physics includes Jolt from.
//
// <Jolt/Jolt.h> must come first in every TU that touches Jolt. All JPH_* configuration macros
// arrive as PUBLIC compile definitions of the Jolt target; never define them here.
#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "aether/core/math.h"
#include "aether/physics/physics_types.h"
#include "aether/scene/entity.h"

#include <type_traits>

namespace aether::physics::detail {

// ---- math conversions ------------------------------------------------------------------------
[[nodiscard]] inline JPH::Vec3 to_jolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
[[nodiscard]] inline JPH::RVec3 to_jolt_r(const Vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
[[nodiscard]] inline JPH::Quat to_jolt(const Quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

[[nodiscard]] inline Vec3 to_ae(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
#ifdef JPH_DOUBLE_PRECISION
[[nodiscard]] inline Vec3 to_ae(JPH::DVec3Arg v) {
    return Vec3(static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ()));
}
#endif
// glm's quaternion constructor argument order depends on configuration macros; set members.
[[nodiscard]] inline Quat to_ae(JPH::QuatArg q) {
    Quat r;
    r.x = q.GetX();
    r.y = q.GetY();
    r.z = q.GetZ();
    r.w = q.GetW();
    return r;
}

// ---- entity <-> Jolt user data ---------------------------------------------------------------
[[nodiscard]] inline JPH::uint64 entity_to_user_data(Entity e) {
    return static_cast<JPH::uint64>(entt::to_integral(e));
}
[[nodiscard]] inline Entity user_data_to_entity(JPH::uint64 data) {
    return static_cast<Entity>(static_cast<std::underlying_type_t<Entity>>(data));
}

// ---- layers ----------------------------------------------------------------------------------
// ObjectLayer = (collision layer << 1) | moving-bit. Two broadphase trees: static and moving.
namespace bp_layers {
inline constexpr JPH::BroadPhaseLayer Static{ 0 };
inline constexpr JPH::BroadPhaseLayer Moving{ 1 };
inline constexpr JPH::uint            Count = 2;
} // namespace bp_layers

[[nodiscard]] inline JPH::ObjectLayer make_object_layer(CollisionLayer layer, bool moving) {
    return static_cast<JPH::ObjectLayer>((static_cast<u32>(layer) << 1) | (moving ? 1u : 0u));
}
[[nodiscard]] inline CollisionLayer collision_layer_of(JPH::ObjectLayer layer) {
    return static_cast<CollisionLayer>(layer >> 1);
}
[[nodiscard]] inline bool is_moving_layer(JPH::ObjectLayer layer) { return (layer & 1u) != 0; }

class BroadPhaseLayerMapper final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return bp_layers::Count; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return is_moving_layer(layer) ? bp_layers::Moving : bp_layers::Static;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp_layers::Moving ? "Moving" : "Static";
    }
#endif
};

// Static objects only need to be tested against the moving tree.
class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        return is_moving_layer(layer) || bp == bp_layers::Moving;
    }
};

// Static-vs-static never collides; everything else consults the gameplay layer matrix.
class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectLayerPairFilterImpl(const CollisionLayerMatrix* matrix) : matrix_(matrix) {}
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!is_moving_layer(a) && !is_moving_layer(b))
            return false;
        return matrix_->collides(collision_layer_of(a), collision_layer_of(b));
    }

private:
    const CollisionLayerMatrix* matrix_;
};

// ---- query filters ---------------------------------------------------------------------------
class QueryBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
    explicit QueryBroadPhaseFilter(const QueryFilter& f) : filter_(f) {}
    [[nodiscard]] bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        return layer == bp_layers::Moving ? filter_.include_moving : filter_.include_static;
    }

private:
    const QueryFilter& filter_;
};

class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryObjectLayerFilter(const QueryFilter& f) : filter_(f) {}
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        const bool moving = is_moving_layer(layer);
        if (moving ? !filter_.include_moving : !filter_.include_static)
            return false;
        const u32 l = collision_layer_of(layer);
        return l < kMaxCollisionLayers && (filter_.layer_mask & (1u << l)) != 0;
    }

private:
    const QueryFilter& filter_;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f)
        : filter_(f), ignore_(f.ignore_entity == kNullEntity ? ~JPH::uint64{ 0 }
                                                              : entity_to_user_data(f.ignore_entity)) {}
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (!filter_.include_sensors && body.IsSensor())
            return false;
        return body.GetUserData() != ignore_;
    }

private:
    const QueryFilter& filter_;
    JPH::uint64        ignore_;
};

} // namespace aether::physics::detail
