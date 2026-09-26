// physics_queries.cpp — raycasts, shape sweeps and overlaps (PhysicsWorld query API).
//
// All queries use Jolt's locking NarrowPhaseQuery, so they are safe to run concurrently with each
// other; they must not overlap sync()/step() (documented on PhysicsWorld).
#include "physics_world_impl.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aether::physics {

using namespace detail;

namespace {

struct CastSetup {
    Vec3 direction{ 0.0f }; // unit
    bool valid = false;
};

[[nodiscard]] CastSetup setup_cast(const Vec3& direction, f32 max_distance) {
    CastSetup  c;
    const f32  len2 = glm::dot(direction, direction);
    if (!(max_distance > 0.0f) || !(len2 > 1.0e-20f) || !std::isfinite(max_distance))
        return c;
    c.direction = direction / std::sqrt(len2);
    c.valid = true;
    return c;
}

[[nodiscard]] Vec3 safe_normal(JPH::Vec3Arg v) {
    return to_ae(v.NormalizedOr(JPH::Vec3::sZero()));
}

// Resolves the entity and the surface normal of a ray hit.
[[nodiscard]] QueryHit make_ray_hit(const JPH::PhysicsSystem& system, const JPH::RRayCast& ray,
                                    const JPH::RayCastResult& result, f32 max_distance) {
    QueryHit          hit;
    const JPH::RVec3  point = ray.GetPointOnRay(result.mFraction);
    hit.point = to_ae(point);
    hit.fraction = result.mFraction;
    hit.distance = result.mFraction * max_distance;
    JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.entity = user_data_to_entity(body.GetUserData());
        hit.normal = safe_normal(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    }
    return hit;
}

[[nodiscard]] Entity entity_of(const JPH::PhysicsSystem& system, const JPH::BodyID& id) {
    JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
    return lock.Succeeded() ? user_data_to_entity(lock.GetBody().GetUserData()) : Entity{ kNullEntity };
}

} // namespace

std::optional<QueryHit> PhysicsWorld::raycast(const Vec3& origin, const Vec3& direction,
                                              f32 max_distance, const QueryFilter& filter) const {
    const CastSetup cast = setup_cast(direction, max_distance);
    if (!cast.valid)
        return std::nullopt;
    const JPH::RRayCast ray{ to_jolt_r(origin), to_jolt(cast.direction * max_distance) };
    JPH::RayCastResult  result;
    const QueryBroadPhaseFilter  bp(filter);
    const QueryObjectLayerFilter ol(filter);
    const QueryBodyFilter        bf(filter);
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result, bp, ol, bf))
        return std::nullopt;
    return make_ray_hit(*impl_->system, ray, result, max_distance);
}

std::vector<QueryHit> PhysicsWorld::raycast_all(const Vec3& origin, const Vec3& direction,
                                                f32 max_distance, const QueryFilter& filter) const {
    std::vector<QueryHit> hits;
    const CastSetup       cast = setup_cast(direction, max_distance);
    if (!cast.valid)
        return hits;
    const JPH::RRayCast ray{ to_jolt_r(origin), to_jolt(cast.direction * max_distance) };
    JPH::RayCastSettings settings; // convex shapes solid, back faces ignored
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const QueryBroadPhaseFilter  bp(filter);
    const QueryObjectLayerFilter ol(filter);
    const QueryBodyFilter        bf(filter);
    impl_->system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, bp, ol, bf);
    collector.Sort(); // by fraction; ties keep Jolt's deterministic order

    hits.reserve(collector.mHits.size());
    std::vector<JPH::BodyID> seen;
    for (const JPH::RayCastResult& r : collector.mHits) {
        if (std::find(seen.begin(), seen.end(), r.mBodyID) != seen.end())
            continue; // one hit (the closest) per body
        seen.push_back(r.mBodyID);
        hits.push_back(make_ray_hit(*impl_->system, ray, r, max_distance));
    }
    return hits;
}

namespace {

std::optional<QueryHit> sweep_shape(const JPH::PhysicsSystem& system, const JPH::Shape& shape,
                                    const Quat& rotation, const Vec3& origin, const Vec3& direction,
                                    f32 max_distance, const QueryFilter& filter) {
    const CastSetup cast = setup_cast(direction, max_distance);
    if (!cast.valid)
        return std::nullopt;

    const Quat rot = glm::dot(rotation, rotation) > 1.0e-12f ? glm::normalize(rotation)
                                                             : Quat(1.0f, 0.0f, 0.0f, 0.0f);
    // Primitive shapes have their centre of mass at the origin, so the query transform is the
    // centre-of-mass transform. Results are relative to the base offset (the cast origin).
    const JPH::RVec3   base = to_jolt_r(origin);
    const JPH::RMat44  start = JPH::RMat44::sRotationTranslation(to_jolt(rot), base);
    const JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(
        &shape, JPH::Vec3::sOne(), start, to_jolt(cast.direction * max_distance));

    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true; // meaningful normal/depth when starting in penetration
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const QueryBroadPhaseFilter  bp(filter);
    const QueryObjectLayerFilter ol(filter);
    const QueryBodyFilter        bf(filter);
    system.GetNarrowPhaseQuery().CastShape(shape_cast, settings, base, collector, bp, ol, bf);
    if (!collector.HadHit())
        return std::nullopt;

    const JPH::ShapeCastResult& r = collector.mHit;
    QueryHit hit;
    hit.entity = entity_of(system, r.mBodyID2);
    hit.point = to_ae(base + r.mContactPointOn2);
    hit.normal = to_ae((-r.mPenetrationAxis).NormalizedOr(JPH::Vec3::sZero()));
    hit.fraction = r.mFraction;
    hit.distance = r.mFraction * max_distance;
    hit.penetration_depth = r.mFraction <= 0.0f ? std::max(r.mPenetrationDepth, 0.0f) : 0.0f;
    return hit;
}

std::vector<QueryHit> overlap_shape(const JPH::PhysicsSystem& system, const JPH::Shape& shape,
                                    const Quat& rotation, const Vec3& center,
                                    const QueryFilter& filter) {
    const Quat rot = glm::dot(rotation, rotation) > 1.0e-12f ? glm::normalize(rotation)
                                                             : Quat(1.0f, 0.0f, 0.0f, 0.0f);
    const JPH::RVec3  base = to_jolt_r(center);
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(to_jolt(rot), base);

    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const QueryBroadPhaseFilter  bp(filter);
    const QueryObjectLayerFilter ol(filter);
    const QueryBodyFilter        bf(filter);
    system.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(), transform, settings, base,
                                              collector, bp, ol, bf);

    // Deepest contact per body.
    std::unordered_map<JPH::uint32, usize> index_of;
    std::vector<QueryHit>                  hits;
    std::vector<f32>                       depth;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const JPH::uint32 key = r.mBodyID2.GetIndexAndSequenceNumber();
        QueryHit h;
        h.point = to_ae(base + r.mContactPointOn2);
        h.normal = to_ae((-r.mPenetrationAxis).NormalizedOr(JPH::Vec3::sZero()));
        h.penetration_depth = r.mPenetrationDepth;
        auto [it, inserted] = index_of.try_emplace(key, hits.size());
        if (inserted) {
            h.entity = entity_of(system, r.mBodyID2);
            hits.push_back(h);
        } else if (r.mPenetrationDepth > hits[it->second].penetration_depth) {
            h.entity = hits[it->second].entity;
            hits[it->second] = h;
        }
    }
    std::sort(hits.begin(), hits.end(), [](const QueryHit& a, const QueryHit& b) {
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    });
    return hits;
}

} // namespace

std::optional<QueryHit> PhysicsWorld::sweep_sphere(f32 radius, const Vec3& origin,
                                                   const Vec3& direction, f32 max_distance,
                                                   const QueryFilter& filter) const {
    if (!(radius > 0.0f))
        return std::nullopt;
    const JPH::SphereShape shape(radius);
    shape.SetEmbedded();
    return sweep_shape(*impl_->system, shape, Quat(1.0f, 0.0f, 0.0f, 0.0f), origin, direction,
                       max_distance, filter);
}

std::optional<QueryHit> PhysicsWorld::sweep_box(const Vec3& half_extents, const Quat& rotation,
                                                const Vec3& origin, const Vec3& direction,
                                                f32 max_distance, const QueryFilter& filter) const {
    const Vec3 he = glm::max(half_extents, Vec3(1.0e-3f));
    const f32  radius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({ he.x, he.y, he.z }));
    const JPH::BoxShape shape(to_jolt(he), radius);
    shape.SetEmbedded();
    return sweep_shape(*impl_->system, shape, rotation, origin, direction, max_distance, filter);
}

std::optional<QueryHit> PhysicsWorld::sweep_capsule(f32 half_height, f32 radius,
                                                    const Quat& rotation, const Vec3& origin,
                                                    const Vec3& direction, f32 max_distance,
                                                    const QueryFilter& filter) const {
    if (!(radius > 0.0f))
        return std::nullopt;
    if (half_height <= 1.0e-4f)
        return sweep_sphere(radius, origin, direction, max_distance, filter);
    const JPH::CapsuleShape shape(half_height, radius);
    shape.SetEmbedded();
    return sweep_shape(*impl_->system, shape, rotation, origin, direction, max_distance, filter);
}

std::vector<QueryHit> PhysicsWorld::overlap_sphere(const Vec3& center, f32 radius,
                                                   const QueryFilter& filter) const {
    if (!(radius > 0.0f))
        return {};
    const JPH::SphereShape shape(radius);
    shape.SetEmbedded();
    return overlap_shape(*impl_->system, shape, Quat(1.0f, 0.0f, 0.0f, 0.0f), center, filter);
}

std::vector<QueryHit> PhysicsWorld::overlap_box(const Vec3& center, const Vec3& half_extents,
                                                const Quat& rotation,
                                                const QueryFilter& filter) const {
    const Vec3 he = glm::max(half_extents, Vec3(1.0e-3f));
    const f32  radius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({ he.x, he.y, he.z }));
    const JPH::BoxShape shape(to_jolt(he), radius);
    shape.SetEmbedded();
    return overlap_shape(*impl_->system, shape, rotation, center, filter);
}

} // namespace aether::physics
