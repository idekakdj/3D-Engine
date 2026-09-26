// physics_world.cpp — PhysicsWorld lifecycle, ECS <-> Jolt synchronisation and stepping.
#include "physics_world_impl.h"

#include "job_system_adapter.h"
#include "shape_builder.h"

#include "aether/core/error.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace aether::physics {

using namespace detail;

// =============================================================================================
// Helpers
// =============================================================================================
namespace detail {

WorldPose decompose(const Mat4& m) {
    WorldPose  out;
    const Vec3 c0(m[0]);
    const Vec3 c1(m[1]);
    const Vec3 c2(m[2]);
    out.position = Vec3(m[3]);
    Vec3 s(glm::length(c0), glm::length(c1), glm::length(c2));
    if (glm::dot(glm::cross(c0, c1), c2) < 0.0f)
        s.x = -s.x; // mirrored basis: fold the reflection into X scale
    const auto safe = [](f32 v) { return std::abs(v) > 1.0e-12f ? v : 1.0f; };
    const Mat3 r(c0 / safe(s.x), c1 / safe(s.y), c2 / safe(s.z));
    out.rotation = glm::normalize(glm::quat_cast(r));
    out.scale = s;
    return out;
}

Mat4 compose(const Vec3& position, const Quat& rotation, const Vec3& scale) {
    return Transform{ position, rotation, scale }.to_matrix();
}

void CharacterListener::OnContactSolve(const JPH::CharacterVirtual* character,
                                       const JPH::BodyID& /*body2*/,
                                       const JPH::SubShapeID& /*sub_shape2*/,
                                       JPH::RVec3Arg /*contact_position*/,
                                       JPH::Vec3Arg contact_normal, JPH::Vec3Arg contact_velocity,
                                       const JPH::PhysicsMaterial* /*contact_material*/,
                                       JPH::Vec3Arg /*character_velocity*/,
                                       JPH::Vec3& new_velocity) {
    const Entity e = user_data_to_entity(character->GetUserData());
    if (!records_->contains(e))
        return;
    if (!records_->get(e).allow_sliding && contact_velocity.IsNearZero() &&
        !character->IsSlopeTooSteep(contact_normal))
        new_velocity = JPH::Vec3::sZero();
}

} // namespace detail

namespace {

[[nodiscard]] u32 entity_id(Entity e) { return static_cast<u32>(entt::to_integral(e)); }

[[nodiscard]] JPH::EMotionType to_jolt_motion(MotionType m) {
    switch (m) {
    case MotionType::Static:    return JPH::EMotionType::Static;
    case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
    case MotionType::Dynamic:   return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

[[nodiscard]] CollisionLayer clamp_layer(CollisionLayer layer, Entity e) {
    if (layer < kMaxCollisionLayers)
        return layer;
    AE_LOG_WARN("Physics", "entity {}: collision layer {} out of range, using 0", entity_id(e), layer);
    return 0;
}

[[nodiscard]] Quat normalize_or_identity(const Quat& q) {
    const f32 len2 = glm::dot(q, q);
    if (len2 < 1.0e-12f)
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    return q / std::sqrt(len2);
}

[[nodiscard]] bool scale_changed(const Vec3& a, const Vec3& b) {
    const Vec3 d = glm::abs(a - b);
    const Vec3 m = glm::max(glm::abs(a), glm::abs(b));
    return d.x > 1.0e-4f * std::max(1.0f, m.x) || d.y > 1.0e-4f * std::max(1.0f, m.y) ||
           d.z > 1.0e-4f * std::max(1.0f, m.z);
}

[[nodiscard]] Vec3 transform_point(const Mat4& m, const Vec3& p) { return Vec3(m * Vec4(p, 1.0f)); }

[[nodiscard]] Vec3 to_body_local(const Pose& pose, const Vec3& world_point) {
    return glm::inverse(pose.rotation) * (world_point - pose.position);
}

[[nodiscard]] GroundState to_ground_state(JPH::CharacterBase::EGroundState s) {
    switch (s) {
    case JPH::CharacterBase::EGroundState::OnGround:      return GroundState::OnGround;
    case JPH::CharacterBase::EGroundState::OnSteepGround: return GroundState::OnSteepGround;
    case JPH::CharacterBase::EGroundState::NotSupported:  return GroundState::NotSupported;
    case JPH::CharacterBase::EGroundState::InAir:         return GroundState::InAir;
    }
    return GroundState::InAir;
}

// ---- constraint settings per component type --------------------------------------------------
struct ConstraintBuild {
    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    Vec3 anchor1{ 0.0f }; // world space
    Vec3 anchor2{ 0.0f }; // world space
};

ConstraintBuild make_constraint(const FixedConstraintComponent&, const Mat4& w1, const Mat4& w2,
                                bool has_target) {
    ConstraintBuild b;
    auto* s = new JPH::FixedConstraintSettings();
    s->mAutoDetectPoint = true; // weld in the current relative pose
    b.settings = s;
    b.anchor1 = Vec3(w1[3]);
    b.anchor2 = has_target ? Vec3(w2[3]) : b.anchor1;
    return b;
}

ConstraintBuild make_constraint(const PointConstraintComponent& c, const Mat4& w1, const Mat4&,
                                bool) {
    ConstraintBuild b;
    auto* s = new JPH::PointConstraintSettings();
    s->mSpace = JPH::EConstraintSpace::WorldSpace;
    b.anchor1 = b.anchor2 = transform_point(w1, c.local_anchor);
    s->mPoint1 = s->mPoint2 = to_jolt_r(b.anchor1);
    b.settings = s;
    return b;
}

ConstraintBuild make_constraint(const HingeConstraintComponent& c, const Mat4& w1, const Mat4&,
                                bool) {
    ConstraintBuild b;
    auto* s = new JPH::HingeConstraintSettings();
    s->mSpace = JPH::EConstraintSpace::WorldSpace;
    b.anchor1 = b.anchor2 = transform_point(w1, c.local_anchor);
    s->mPoint1 = s->mPoint2 = to_jolt_r(b.anchor1);

    Vec3 axis = Mat3(w1) * c.local_axis;
    axis = glm::dot(axis, axis) > 1.0e-12f ? glm::normalize(axis) : Vec3(0.0f, 1.0f, 0.0f);
    const Vec3 pick = std::abs(axis.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 normal = glm::normalize(glm::cross(axis, pick));
    s->mHingeAxis1 = s->mHingeAxis2 = to_jolt(axis);
    s->mNormalAxis1 = s->mNormalAxis2 = to_jolt(normal);
    if (c.limits_enabled) {
        s->mLimitsMin = glm::radians(std::clamp(c.min_angle_deg, -180.0f, 0.0f));
        s->mLimitsMax = glm::radians(std::clamp(c.max_angle_deg, 0.0f, 180.0f));
    }
    s->mMaxFrictionTorque = std::max(c.max_friction_torque, 0.0f);
    b.settings = s;
    return b;
}

ConstraintBuild make_constraint(const DistanceConstraintComponent& c, const Mat4& w1,
                                const Mat4& w2, bool has_target) {
    ConstraintBuild b;
    auto* s = new JPH::DistanceConstraintSettings();
    s->mSpace = JPH::EConstraintSpace::WorldSpace;
    b.anchor1 = transform_point(w1, c.local_anchor);
    b.anchor2 = has_target ? transform_point(w2, c.target_local_anchor) : c.target_local_anchor;
    s->mPoint1 = to_jolt_r(b.anchor1);
    s->mPoint2 = to_jolt_r(b.anchor2);
    f32 min_d = c.min_distance;
    f32 max_d = c.max_distance;
    if (min_d >= 0.0f && max_d >= 0.0f && min_d > max_d)
        std::swap(min_d, max_d);
    s->mMinDistance = min_d; // negative => current distance (Jolt, world space only)
    s->mMaxDistance = max_d;
    if (c.spring_frequency > 0.0f) {
        s->mLimitsSpringSettings.mFrequency = c.spring_frequency;
        s->mLimitsSpringSettings.mDamping = std::max(c.spring_damping, 0.0f);
    }
    b.settings = s;
    return b;
}

} // namespace

// =============================================================================================
// Impl: construction / destruction
// =============================================================================================
PhysicsWorld::Impl::Impl(World& w, const PhysicsWorldSettings& s)
    : world(w), reg(w.registry()), settings(s), pair_filter(&settings.layers),
      character_listener(&characters_) {
    temp_allocator = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(
        std::max(settings.temp_allocator_bytes, 1024u * 1024u));

    PhysicsJobBackend backend = settings.job_backend;
    if (backend == PhysicsJobBackend::Auto)
        backend = aether::JobSystem::worker_count() > 0 ? PhysicsJobBackend::EngineJobSystem
                                                : PhysicsJobBackend::JoltThreadPool;
    const JPH::uint max_jobs = std::max(settings.max_jobs, 64u);
    const JPH::uint max_barriers = std::max(settings.max_barriers, 2u);
    switch (backend) {
    case PhysicsJobBackend::EngineJobSystem:
        job_system = std::make_unique<EngineJobSystemAdapter>(max_jobs, max_barriers);
        break;
    case PhysicsJobBackend::JoltThreadPool: {
        int threads = settings.worker_threads;
        if (threads < 0)
            threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        job_system = std::make_unique<JPH::JobSystemThreadPool>(max_jobs, max_barriers, threads);
        break;
    }
    case PhysicsJobBackend::SingleThreaded:
    case PhysicsJobBackend::Auto:
        job_system = std::make_unique<JPH::JobSystemSingleThreaded>(max_jobs);
        break;
    }

    system = std::make_unique<JPH::PhysicsSystem>();
    system->Init(settings.max_bodies, settings.num_body_mutexes, settings.max_body_pairs,
                 settings.max_contact_constraints, bp_mapper, obj_vs_bp_filter, pair_filter);
    system->SetGravity(to_jolt(settings.gravity));
    JPH::PhysicsSettings physics_settings = system->GetPhysicsSettings();
    physics_settings.mPenetrationSlop = std::max(settings.penetration_slop, 0.0f);
    system->SetPhysicsSettings(physics_settings);
    system->SetContactListener(&recorder);

    // Body lifecycle follows the ECS.
    connections.push_back(reg.on_construct<RigidBodyComponent>().connect<&Impl::on_rigid_changed>(*this));
    connections.push_back(reg.on_update<RigidBodyComponent>().connect<&Impl::on_rigid_changed>(*this));
    connections.push_back(reg.on_destroy<RigidBodyComponent>().connect<&Impl::on_rigid_removed>(*this));
    connections.push_back(reg.on_construct<ColliderComponent>().connect<&Impl::on_rigid_changed>(*this));
    connections.push_back(reg.on_update<ColliderComponent>().connect<&Impl::on_rigid_changed>(*this));
    connections.push_back(reg.on_destroy<ColliderComponent>().connect<&Impl::on_rigid_removed>(*this));
    connections.push_back(reg.on_construct<CharacterControllerComponent>().connect<&Impl::on_character_changed>(*this));
    connections.push_back(reg.on_update<CharacterControllerComponent>().connect<&Impl::on_character_changed>(*this));
    connections.push_back(reg.on_destroy<CharacterControllerComponent>().connect<&Impl::on_character_changed>(*this));

    const auto connect_constraint = [this]<typename C, ConstraintKind K>() {
        connections.push_back(reg.on_construct<C>().template connect<&Impl::on_constraint_changed<K>>(*this));
        connections.push_back(reg.on_update<C>().template connect<&Impl::on_constraint_changed<K>>(*this));
        connections.push_back(reg.on_destroy<C>().template connect<&Impl::on_constraint_changed<K>>(*this));
    };
    connect_constraint.operator()<FixedConstraintComponent, ConstraintKind::Fixed>();
    connect_constraint.operator()<PointConstraintComponent, ConstraintKind::Point>();
    connect_constraint.operator()<HingeConstraintComponent, ConstraintKind::Hinge>();
    connect_constraint.operator()<DistanceConstraintComponent, ConstraintKind::Distance>();

    // Components that existed before physics started (e.g. a scene loaded earlier).
    for (const Entity e : reg.view<ColliderComponent>())
        enqueue(e);
    for (const Entity e : reg.view<CharacterControllerComponent>())
        enqueue(e);
}

PhysicsWorld::Impl::~Impl() {
    for (entt::connection& c : connections)
        c.release();
    connections.clear();

    // Constraints reference bodies; characters own their inner bodies.
    for (auto& [key, rec] : constraints_)
        if (rec.constraint != nullptr)
            system->RemoveConstraint(rec.constraint);
    constraints_.clear();
    characters_.clear();

    std::vector<JPH::BodyID> ids;
    ids.reserve(bodies_.size());
    for (auto [e, rec] : bodies_.each())
        ids.push_back(rec.id);
    if (!ids.empty()) {
        JPH::BodyInterface& bi = bodies();
        bi.RemoveBodies(ids.data(), static_cast<int>(ids.size()));
        bi.DestroyBodies(ids.data(), static_cast<int>(ids.size()));
    }
    bodies_.clear();

    system.reset();
    job_system.reset(); // waits for engine-side jobs still referencing it
    temp_allocator.reset();
}

// =============================================================================================
// Impl: lifecycle
// =============================================================================================
void PhysicsWorld::Impl::enqueue(Entity e) {
    if (!pending_set_.contains(e)) {
        pending_set_.push(e);
        pending_.push_back(e);
    }
}

void PhysicsWorld::Impl::invalidate_body(Entity e, bool preserve_velocity) {
    AE_ASSERT(!stepping);
    destroy_body(e, preserve_velocity);
    enqueue(e);
}

void PhysicsWorld::Impl::invalidate_character(Entity e) {
    AE_ASSERT(!stepping);
    destroy_character(e);
    enqueue(e);
}

void PhysicsWorld::Impl::destroy_body(Entity e, bool preserve_velocity) {
    BodyRecord* rec = body_record(e);
    if (rec == nullptr)
        return;
    JPH::BodyInterface& bi = bodies();
    if (preserve_velocity && rec->motion == MotionType::Dynamic) {
        JPH::Vec3 lin;
        JPH::Vec3 ang;
        bi.GetLinearAndAngularVelocity(rec->id, lin, ang);
        preserved_.remove(e);
        preserved_.emplace(e, PreservedVelocity{ to_ae(lin), to_ae(ang) });
    }
    detach_constraints_of(rec->id);

    // Jolt doesn't wake bodies resting on a removed body; do it so nothing floats in mid-air.
    JPH::AABox bounds = bi.GetTransformedShape(rec->id).GetWorldSpaceBounds();
    bounds.ExpandBy(JPH::Vec3::sReplicate(0.05f));
    bi.RemoveBody(rec->id);
    bi.DestroyBody(rec->id);
    bi.ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});

    moving_.remove(e);
    bodies_.erase(e);
}

void PhysicsWorld::Impl::destroy_character(Entity e) {
    if (characters_.contains(e))
        characters_.erase(e); // CharacterVirtual's destructor removes its inner body
}

void PhysicsWorld::Impl::create_pending() {
    if (pending_.empty())
        return;
    std::vector<Entity> work;
    work.swap(pending_);
    pending_set_.clear();

    std::vector<JPH::BodyID> activate;
    std::vector<JPH::BodyID> dont_activate;
    for (const Entity e : work) {
        if (reg.valid(e)) {
            create_character(e);
            create_body(e, activate, dont_activate);
        }
        preserved_.remove(e);
    }

    // Batch insertion keeps the broadphase trees well balanced (Jolt recommendation).
    JPH::BodyInterface& bi = bodies();
    if (!activate.empty()) {
        const int n = static_cast<int>(activate.size());
        const auto state = bi.AddBodiesPrepare(activate.data(), n);
        bi.AddBodiesFinalize(activate.data(), n, state, JPH::EActivation::Activate);
    }
    if (!dont_activate.empty()) {
        const int n = static_cast<int>(dont_activate.size());
        const auto state = bi.AddBodiesPrepare(dont_activate.data(), n);
        bi.AddBodiesFinalize(dont_activate.data(), n, state, JPH::EActivation::DontActivate);
    }
}

void PhysicsWorld::Impl::create_body(Entity e, std::vector<JPH::BodyID>& activate,
                                     std::vector<JPH::BodyID>& dont_activate) {
    if (bodies_.contains(e))
        return;
    const auto* collider = reg.try_get<ColliderComponent>(e);
    if (collider == nullptr || !reg.all_of<TransformComponent>(e))
        return;
    if (reg.all_of<CharacterControllerComponent>(e)) {
        AE_LOG_WARN("Physics",
                    "entity {}: ColliderComponent ignored (entity has a CharacterControllerComponent)",
                    entity_id(e));
        return;
    }

    RigidBodyComponent implicit_static;
    implicit_static.motion_type = MotionType::Static;
    const auto*               rb_ptr = reg.try_get<RigidBodyComponent>(e);
    const RigidBodyComponent& rb = rb_ptr != nullptr ? *rb_ptr : implicit_static;

    MotionType motion = rb.motion_type;
    if (collider->shape == ColliderShape::TriangleMesh && motion != MotionType::Static) {
        AE_LOG_WARN("Physics", "entity {}: triangle-mesh colliders are static-only; body made static",
                    entity_id(e));
        motion = MotionType::Static;
    }
    const auto allowed = static_cast<u8>(0x3Fu & ~static_cast<u32>(rb.locked_dofs));
    if (motion == MotionType::Dynamic && allowed == 0) {
        AE_LOG_WARN("Physics", "entity {}: all degrees of freedom locked; body made static",
                    entity_id(e));
        motion = MotionType::Static;
    }

    const WorldPose pose = world_pose_of(e);
    BuiltShape      built = build_collider_shape(*collider, pose.scale);
    if (built.shape == nullptr) {
        AE_LOG_ERROR("Physics", "entity {}: collider creation failed: {}", entity_id(e), built.error);
        return;
    }

    const bool moving = motion != MotionType::Static;
    const CollisionLayer layer = clamp_layer(rb.collision_layer, e);
    JPH::BodyCreationSettings bcs(built.shape.GetPtr(), to_jolt_r(pose.position),
                                  to_jolt(pose.rotation), to_jolt_motion(motion),
                                  make_object_layer(layer, moving));
    bcs.mUserData = entity_to_user_data(e);
    bcs.mFriction = rb.friction;
    bcs.mRestitution = rb.restitution;
    bcs.mLinearDamping = std::max(rb.linear_damping, 0.0f);
    bcs.mAngularDamping = std::max(rb.angular_damping, 0.0f);
    bcs.mGravityFactor = rb.gravity_factor;
    bcs.mIsSensor = rb.is_sensor;
    bcs.mAllowSleeping = rb.allow_sleeping;
    bcs.mMotionQuality =
        rb.continuous_collision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    if (motion == MotionType::Dynamic) {
        bcs.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(allowed);
        if (rb.mass > 0.0f) {
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = rb.mass;
        }
        if (preserved_.contains(e)) {
            const PreservedVelocity& v = preserved_.get(e);
            bcs.mLinearVelocity = to_jolt(v.linear);
            bcs.mAngularVelocity = to_jolt(v.angular);
        } else {
            bcs.mLinearVelocity = to_jolt(rb.initial_linear_velocity);
            bcs.mAngularVelocity = to_jolt(rb.initial_angular_velocity);
        }
    }

    JPH::Body* body = bodies().CreateBody(bcs);
    if (body == nullptr) {
        AE_LOG_ERROR("Physics", "body limit reached ({}); entity {} gets no body",
                     settings.max_bodies, entity_id(e));
        return;
    }

    BodyRecord rec;
    rec.id = body->GetID();
    rec.motion = motion;
    rec.sensor = rb.is_sensor;
    rec.scale = pose.scale;
    rec.sync = capture_sync(e);
    rec.prev = rec.curr = Pose{ pose.position, pose.rotation };
    rec.base_shape = built.base;
    const JPH::BodyID id = rec.id;
    bodies_.emplace(e, std::move(rec));
    if (moving)
        moving_.push(e);
    (moving ? activate : dont_activate).push_back(id);
}

void PhysicsWorld::Impl::create_character(Entity e) {
    if (characters_.contains(e))
        return;
    const auto* cc = reg.try_get<CharacterControllerComponent>(e);
    if (cc == nullptr || !reg.all_of<TransformComponent>(e))
        return;

    const WorldPose       pose = world_pose_of(e);
    const f32             radius = std::max(cc->radius, 1.0e-3f);
    const f32             half_height = std::max(cc->half_height, 0.0f);
    const CollisionLayer  layer = clamp_layer(cc->collision_layer, e);
    const JPH::ObjectLayer object_layer = make_object_layer(layer, true);

    JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
    cs->mShape = build_character_shape(half_height, radius);
    cs->mMaxSlopeAngle = glm::radians(std::clamp(cc->max_slope_deg, 0.0f, 89.0f));
    cs->mMaxStrength = std::max(cc->max_strength, 0.0f);
    cs->mMass = std::max(cc->mass, 0.0f);
    // Only contacts below the centre of the bottom hemisphere can support the character.
    cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    // Inner rigid body: gives the character presence for queries, sensors and rigid bodies.
    cs->mInnerBodyShape = cs->mShape;
    cs->mInnerBodyLayer = object_layer;

    CharacterRecord rec;
    rec.character = new JPH::CharacterVirtual(cs, to_jolt_r(pose.position), JPH::Quat::sIdentity(),
                                              entity_to_user_data(e), system.get());
    rec.character->SetListener(&character_listener);
    rec.synced_position = pose.position;
    rec.prev.position = rec.curr.position = pose.position;
    rec.radius = radius;
    rec.half_height = half_height;

    const auto              bp_filter = system->GetDefaultBroadPhaseLayerFilter(object_layer);
    const auto              layer_filter = system->GetDefaultLayerFilter(object_layer);
    const JPH::BodyFilter   body_filter;
    const JPH::ShapeFilter  shape_filter;
    rec.character->RefreshContacts(bp_filter, layer_filter, body_filter, shape_filter, *temp_allocator);
    characters_.emplace(e, std::move(rec));
}

// =============================================================================================
// Impl: constraints
// =============================================================================================
u64 PhysicsWorld::Impl::constraint_key(Entity e, ConstraintKind kind) {
    return (static_cast<u64>(entt::to_integral(e)) << 8) | static_cast<u64>(kind);
}

void PhysicsWorld::Impl::erase_constraint(Entity e, ConstraintKind kind) {
    AE_ASSERT(!stepping);
    const auto it = constraints_.find(constraint_key(e, kind));
    if (it == constraints_.end())
        return;
    if (it->second.constraint != nullptr) {
        system->RemoveConstraint(it->second.constraint);
        JPH::BodyInterface& bi = bodies();
        for (const JPH::BodyID& id : { it->second.body1, it->second.body2 })
            if (!id.IsInvalid() && bi.IsAdded(id))
                bi.ActivateBody(id);
    }
    constraints_.erase(it);
}

void PhysicsWorld::Impl::detach_constraints_of(const JPH::BodyID& body) {
    JPH::BodyInterface& bi = bodies();
    for (auto& [key, rec] : constraints_) {
        if (rec.constraint == nullptr || (rec.body1 != body && rec.body2 != body))
            continue;
        system->RemoveConstraint(rec.constraint);
        rec.constraint = nullptr; // recreated on a later sync if both bodies exist again
        for (const JPH::BodyID& id : { rec.body1, rec.body2 })
            if (id != body && !id.IsInvalid() && bi.IsAdded(id))
                bi.ActivateBody(id); // the survivor may now be unsupported
    }
}

template <typename C, ConstraintKind Kind>
void PhysicsWorld::Impl::create_constraints_of() {
    for (auto [e, comp] : reg.view<C>().each()) {
        auto [it, inserted] = constraints_.try_emplace(constraint_key(e, Kind));
        ConstraintRecord& rec = it->second;
        if (inserted) {
            rec.owner = e;
            rec.kind = Kind;
        }
        if (rec.constraint != nullptr)
            continue;

        const BodyRecord* b1 = body_record(e);
        if (b1 == nullptr)
            continue; // owner body not created (yet)
        const Entity      target = comp.target;
        const BodyRecord* b2 = nullptr;
        if (target != kNullEntity) {
            if (target == e || !reg.valid(target))
                continue;
            b2 = body_record(target);
            if (b2 == nullptr)
                continue; // wait for the target's body
        }

        const Mat4 w1 = world_matrix_of(e);
        const Mat4 w2 = b2 != nullptr ? world_matrix_of(target) : Mat4(1.0f);
        ConstraintBuild build = make_constraint(comp, w1, w2, b2 != nullptr);
        const JPH::BodyID id2 = b2 != nullptr ? b2->id : JPH::BodyID();

        JPH::TwoBodyConstraint* c = bodies().CreateConstraint(build.settings, b1->id, id2);
        if (c == nullptr)
            continue;
        system->AddConstraint(c);
        bodies().ActivateConstraint(c);

        rec.target = target;
        rec.constraint = c;
        rec.body1 = b1->id;
        rec.body2 = id2;
        rec.anchor1_local = to_body_local(b1->curr, build.anchor1);
        rec.anchor2_local = b2 != nullptr ? to_body_local(b2->curr, build.anchor2) : build.anchor2;
    }
}

void PhysicsWorld::Impl::create_constraints() {
    create_constraints_of<FixedConstraintComponent, ConstraintKind::Fixed>();
    create_constraints_of<PointConstraintComponent, ConstraintKind::Point>();
    create_constraints_of<HingeConstraintComponent, ConstraintKind::Hinge>();
    create_constraints_of<DistanceConstraintComponent, ConstraintKind::Distance>();
}

// =============================================================================================
// Impl: transforms
// =============================================================================================
Entity PhysicsWorld::Impl::parent_of(Entity e) const {
    const auto* h = reg.try_get<HierarchyComponent>(e);
    return h != nullptr ? h->parent : Entity{ kNullEntity };
}

u32 PhysicsWorld::Impl::depth_of(Entity e) const {
    u32    depth = 0;
    Entity p = parent_of(e);
    while (p != kNullEntity && depth < 256 && reg.valid(p)) {
        ++depth;
        p = parent_of(p);
    }
    return depth;
}

// Composes local TRS up the hierarchy. Independent of the scene's cached world matrices, so it
// is exact even for transforms written earlier in the same write-back pass.
Mat4 PhysicsWorld::Impl::world_matrix_of(Entity e) const {
    const auto* tc = reg.try_get<TransformComponent>(e);
    if (tc == nullptr)
        return Mat4(1.0f);
    Mat4   m = tc->local.to_matrix();
    Entity p = parent_of(e);
    u32    guard = 0;
    while (p != kNullEntity && guard++ < 256 && reg.valid(p)) {
        if (const auto* ptc = reg.try_get<TransformComponent>(p))
            m = ptc->local.to_matrix() * m;
        p = parent_of(p);
    }
    return m;
}

WorldPose PhysicsWorld::Impl::world_pose_of(Entity e) const {
    const auto* tc = reg.try_get<TransformComponent>(e);
    if (tc == nullptr)
        return {};
    if (parent_of(e) == kNullEntity)
        return { tc->local.position, normalize_or_identity(tc->local.rotation), tc->local.scale };
    WorldPose p = decompose(world_matrix_of(e));
    return p;
}

SyncState PhysicsWorld::Impl::capture_sync(Entity e) const {
    SyncState s;
    if (const auto* tc = reg.try_get<TransformComponent>(e))
        s.local = tc->local;
    s.parent = parent_of(e);
    s.world = world_matrix_of(e);
    return s;
}

bool PhysicsWorld::Impl::transform_edited(Entity e, const SyncState& sync) const {
    const auto* tc = reg.try_get<TransformComponent>(e);
    if (tc == nullptr)
        return false;
    const Entity parent = parent_of(e);
    if (parent != sync.parent)
        return true;
    if (parent == kNullEntity) // bitwise: we wrote these exact values
        return std::memcmp(&tc->local, &sync.local, sizeof(Transform)) != 0;
    const Mat4 w = world_matrix_of(e);
    return std::memcmp(&w, &sync.world, sizeof(Mat4)) != 0;
}

SyncState PhysicsWorld::Impl::write_back(Entity e, const Pose& pose, bool write_rotation) {
    auto* tc = reg.try_get<TransformComponent>(e);
    if (tc == nullptr)
        return {};
    const Entity parent = parent_of(e);
    if (parent == kNullEntity) {
        tc->local.position = pose.position;
        if (write_rotation)
            tc->local.rotation = pose.rotation;
    } else {
        const Mat4 parent_world = world_matrix_of(parent);
        tc->local.position = transform_point(glm::inverse(parent_world), pose.position);
        if (write_rotation) {
            // Exact for uniformly scaled parents; non-uniform parent scale can't be represented
            // in a child TRS anyway.
            const WorldPose pp = decompose(parent_world);
            tc->local.rotation = glm::normalize(glm::inverse(pp.rotation) * pose.rotation);
        }
    }
    tc->world = world_matrix_of(e);
    tc->dirty = true;

    SyncState s;
    s.local = tc->local;
    s.parent = parent;
    s.world = tc->world;
    return s;
}

// =============================================================================================
// Impl: stepping
// =============================================================================================
void PhysicsWorld::Impl::teleport(Entity e, BodyRecord& rec, std::vector<Entity>& rebuilds) {
    const WorldPose wp = world_pose_of(e);
    if (scale_changed(rec.scale, wp.scale)) {
        rebuilds.push_back(e); // shapes bake scale in: rebuild
        return;
    }
    JPH::BodyInterface& bi = bodies();
    const bool          is_static = rec.motion == MotionType::Static;
    JPH::AABox          region;
    if (is_static)
        region = bi.GetTransformedShape(rec.id).GetWorldSpaceBounds();
    bi.SetPositionAndRotation(rec.id, to_jolt_r(wp.position), to_jolt(wp.rotation),
                              is_static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (is_static) {
        // Moving level geometry must wake whatever was resting on / near it.
        region.Encapsulate(bi.GetTransformedShape(rec.id).GetWorldSpaceBounds());
        region.ExpandBy(JPH::Vec3::sReplicate(0.05f));
        bi.ActivateBodiesInAABox(region, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});
    }
    rec.prev = rec.curr = Pose{ wp.position, wp.rotation };
    rec.kinematic_moving = false;
    rec.sync = capture_sync(e);
}

void PhysicsWorld::Impl::push_ecs_edits(f32 dt, std::vector<Entity>& rebuilds) {
    JPH::BodyInterface& bi = bodies();
    for (const Entity e : moving_) {
        BodyRecord& rec = bodies_.get(e);
        const bool  edited = transform_edited(e, rec.sync);
        if (rec.motion == MotionType::Kinematic && dt > 0.0f) {
            if (edited) {
                const WorldPose wp = world_pose_of(e);
                if (scale_changed(rec.scale, wp.scale)) {
                    rebuilds.push_back(e);
                    continue;
                }
                bi.MoveKinematic(rec.id, to_jolt_r(wp.position), to_jolt(wp.rotation), dt);
                rec.sync = capture_sync(e);
                rec.kinematic_moving = true;
            } else if (rec.kinematic_moving) {
                // Kinematic bodies keep their velocity until told otherwise.
                bi.SetLinearAndAngularVelocity(rec.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                rec.kinematic_moving = false;
            }
        } else if (edited) {
            teleport(e, rec, rebuilds);
        }
    }

    if (settings.track_static_transforms) {
        for (auto [e, rec] : bodies_.each())
            if (rec.motion == MotionType::Static && transform_edited(e, rec.sync))
                teleport(e, rec, rebuilds);
    }
}

void PhysicsWorld::Impl::sync_internal(f32 dt) {
    AE_ASSERT(!stepping);
    create_pending();
    std::vector<Entity> rebuilds;
    push_ecs_edits(dt, rebuilds);
    if (!rebuilds.empty()) {
        for (const Entity e : rebuilds) {
            destroy_body(e, true);
            enqueue(e);
        }
        create_pending();
    }
    create_constraints();
}

void PhysicsWorld::Impl::update_characters(f32 dt) {
    const JPH::Vec3 gravity = system->GetGravity();
    const JPH::Vec3 up = JPH::Vec3::sAxisY();
    for (auto [e, rec] : characters_.each()) {
        auto* cc = reg.try_get<CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        JPH::CharacterVirtual& ch = *rec.character;
        const JPH::ObjectLayer object_layer = make_object_layer(clamp_layer(cc->collision_layer, e), true);
        const auto             bp_filter = system->GetDefaultBroadPhaseLayerFilter(object_layer);
        const auto             layer_filter = system->GetDefaultLayerFilter(object_layer);
        const JPH::BodyFilter  body_filter;
        const JPH::ShapeFilter shape_filter;

        // Gameplay/editor moved the entity: teleport the character.
        const Vec3 world_position(world_matrix_of(e)[3]);
        if (std::memcmp(&world_position, &rec.synced_position, sizeof(Vec3)) != 0) {
            ch.SetPosition(to_jolt_r(world_position));
            ch.RefreshContacts(bp_filter, layer_filter, body_filter, shape_filter, *temp_allocator);
            rec.prev.position = rec.curr.position = world_position;
            rec.synced_position = world_position;
        }
        if (dt <= 0.0f)
            continue;

        Vec3 desired = cc->desired_velocity;
        desired.y = 0.0f;
        const bool wants_move = glm::dot(desired, desired) > 1.0e-8f;

        // Standard Jolt character velocity model (see Samples/Tests/Character).
        ch.UpdateGroundVelocity();
        const JPH::Vec3 desired_velocity = ch.CancelVelocityTowardsSteepSlopes(to_jolt(desired));
        const JPH::Vec3 current_vertical = up * ch.GetLinearVelocity().Dot(up);
        const JPH::Vec3 ground_velocity = ch.GetGroundVelocity();
        const bool moving_towards_ground = (current_vertical.GetY() - ground_velocity.GetY()) < 0.1f;

        JPH::Vec3 new_velocity;
        bool      jumped = false;
        if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && moving_towards_ground) {
            new_velocity = ground_velocity;
            if (cc->jump_requested) {
                new_velocity += up * cc->jump_speed;
                jumped = true;
            }
        } else {
            new_velocity = current_vertical;
        }
        cc->jump_requested = false;
        if (cc->apply_gravity)
            new_velocity += gravity * dt;
        new_velocity += desired_velocity;

        rec.allow_sliding = wants_move || jumped || !ch.IsSupported();
        ch.SetLinearVelocity(new_velocity);

        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(cc->stick_to_floor_distance, 0.0f), 0.0f);
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(cc->step_up_height, 0.0f), 0.0f);
        ch.ExtendedUpdate(dt, gravity, update, bp_filter, layer_filter, body_filter, shape_filter,
                          *temp_allocator);
    }
}

void PhysicsWorld::Impl::pull_results() {
    struct WriteItem {
        u32    depth;
        Entity e;
        bool   character;
        bool   moved;
    };
    std::vector<WriteItem> writes;
    bool                   any_parented = false;

    const JPH::BodyInterface& bi = system->GetBodyInterfaceNoLock(); // main thread, no step running
    for (const Entity e : moving_) {
        BodyRecord& rec = bodies_.get(e);
        JPH::RVec3  p;
        JPH::Quat   q;
        bi.GetPositionAndRotation(rec.id, p, q);
        const Pose now{ to_ae(p), to_ae(q) };
        const bool moved = std::memcmp(&now, &rec.curr, sizeof(Pose)) != 0;
        rec.prev = rec.curr;
        rec.curr = now;
        if (rec.motion != MotionType::Dynamic)
            continue; // kinematic bodies are driven by the ECS, never written back
        const u32 depth = parent_of(e) != kNullEntity ? depth_of(e) : 0u;
        if (moved || depth > 0) {
            writes.push_back({ depth, e, false, moved });
            any_parented = any_parented || depth > 0;
        }
    }

    for (auto [e, rec] : characters_.each()) {
        const JPH::CharacterVirtual& ch = *rec.character;
        const Vec3                   now = to_ae(ch.GetPosition());
        const bool                   moved = std::memcmp(&now, &rec.curr.position, sizeof(Vec3)) != 0;
        rec.prev = rec.curr;
        rec.curr.position = now;

        if (auto* cc = reg.try_get<CharacterControllerComponent>(e)) {
            const auto state = ch.GetGroundState();
            cc->ground_state = to_ground_state(state);
            cc->on_ground = state == JPH::CharacterBase::EGroundState::OnGround;
            cc->ground_normal = state == JPH::CharacterBase::EGroundState::InAir
                                    ? Vec3(0.0f)
                                    : to_ae(ch.GetGroundNormal());
            cc->velocity = to_ae(ch.GetLinearVelocity());
            cc->ground_entity = ch.GetGroundBodyID().IsInvalid()
                                    ? Entity{ kNullEntity }
                                    : user_data_to_entity(ch.GetGroundUserData());
        }
        const u32 depth = parent_of(e) != kNullEntity ? depth_of(e) : 0u;
        if (moved || depth > 0) {
            writes.push_back({ depth, e, true, moved });
            any_parented = any_parented || depth > 0;
        }
    }

    // Parents before children so each child is expressed relative to its parent's NEW pose.
    if (any_parented)
        std::stable_sort(writes.begin(), writes.end(),
                         [](const WriteItem& a, const WriteItem& b) { return a.depth < b.depth; });

    for (const WriteItem& w : writes) {
        if (w.character) {
            CharacterRecord& rec = characters_.get(w.e);
            if (!w.moved && Vec3(world_matrix_of(w.e)[3]) == rec.synced_position)
                continue;
            const SyncState s = write_back(w.e, Pose{ rec.curr.position }, false);
            rec.synced_position = Vec3(s.world[3]);
        } else {
            BodyRecord& rec = bodies_.get(w.e);
            // Unmoved parented bodies only need rewriting if an ancestor was just written.
            if (!w.moved && !transform_edited(w.e, rec.sync))
                continue;
            rec.sync = write_back(w.e, rec.curr, true);
        }
    }
}

// =============================================================================================
// PhysicsWorld
// =============================================================================================
PhysicsWorld::PhysicsWorld(World& world, const PhysicsWorldSettings& settings)
    : impl_(std::make_unique<Impl>(world, settings)) {}

PhysicsWorld::~PhysicsWorld() = default;

World&                      PhysicsWorld::world() const { return impl_->world; }
const PhysicsWorldSettings& PhysicsWorld::settings() const { return impl_->settings; }

void PhysicsWorld::sync() { impl_->sync_internal(0.0f); }

void PhysicsWorld::step(f32 dt) {
    Impl& d = *impl_;
    if (!(dt > 0.0f)) {
        AE_LOG_WARN("Physics", "PhysicsWorld::step ignored non-positive dt {}", dt);
        return;
    }
    d.sync_internal(dt);
    if (!d.broadphase_optimized && !d.bodies_.empty()) {
        d.system->OptimizeBroadPhase();
        d.broadphase_optimized = true;
    }

    d.update_characters(dt);

    d.stepping = true;
    const JPH::EPhysicsUpdateError err =
        d.system->Update(dt, static_cast<int>(std::max(d.settings.collision_steps, 1u)),
                         d.temp_allocator.get(), d.job_system.get());
    d.stepping = false;
    ++d.step_count;

    if (err != JPH::EPhysicsUpdateError::None) {
        const u32 bits = static_cast<u32>(err);
        if ((bits & ~d.reported_update_errors) != 0) {
            d.reported_update_errors |= bits;
            AE_LOG_WARN("Physics",
                        "Jolt buffers overflowed (EPhysicsUpdateError 0x{:x}); raise max_body_pairs "
                        "/ max_contact_constraints",
                        bits);
        }
    }

    d.pull_results();
    d.recorder.take(d.raw_contacts);
    d.tracker.process(d.raw_contacts, d.step_count, d.settings.emit_persist_events, d.events,
                      d.settings.max_buffered_events, d.debug_contacts);
}

u64 PhysicsWorld::step_count() const { return impl_->step_count; }

void PhysicsWorld::set_gravity(const Vec3& gravity) {
    impl_->settings.gravity = gravity;
    impl_->system->SetGravity(to_jolt(gravity));
}

Vec3 PhysicsWorld::gravity() const { return to_ae(impl_->system->GetGravity()); }

void PhysicsWorld::set_layer_collision(CollisionLayer a, CollisionLayer b, bool collides) {
    AE_ASSERT(!impl_->stepping);
    impl_->settings.layers.set_collides(a, b, collides);
}

bool PhysicsWorld::layers_collide(CollisionLayer a, CollisionLayer b) const {
    return impl_->settings.layers.collides(a, b);
}

void PhysicsWorld::optimize_broadphase() {
    impl_->system->OptimizeBroadPhase();
    impl_->broadphase_optimized = true;
}

bool PhysicsWorld::has_body(Entity e) const { return impl_->bodies_.contains(e); }
bool PhysicsWorld::has_character(Entity e) const { return impl_->characters_.contains(e); }
usize PhysicsWorld::body_count() const { return impl_->bodies_.size(); }
usize PhysicsWorld::character_count() const { return impl_->characters_.size(); }

usize PhysicsWorld::constraint_count() const {
    usize n = 0;
    for (const auto& [key, rec] : impl_->constraints_)
        n += rec.constraint != nullptr ? 1u : 0u;
    return n;
}

usize PhysicsWorld::active_body_count() const {
    const usize active = impl_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    const usize inner = impl_->characters_.size(); // character inner bodies never sleep
    return active > inner ? active - inner : 0u;
}

bool PhysicsWorld::is_awake(Entity e) const {
    const BodyRecord* rec = impl_->body_record(e);
    return rec != nullptr && impl_->bodies().IsActive(rec->id);
}

std::optional<MotionType> PhysicsWorld::motion_type(Entity e) const {
    const BodyRecord* rec = impl_->body_record(e);
    if (rec == nullptr)
        return std::nullopt;
    return rec->motion;
}

void PhysicsWorld::wake_up(Entity e) {
    if (const BodyRecord* rec = impl_->body_record(e))
        impl_->bodies().ActivateBody(rec->id);
}

void PhysicsWorld::rebuild_body(Entity e) {
    Impl& d = *impl_;
    d.invalidate_body(e, true);
    d.invalidate_character(e);
    for (u8 k = 0; k <= static_cast<u8>(ConstraintKind::Distance); ++k)
        d.erase_constraint(e, static_cast<ConstraintKind>(k));
}

std::optional<Transform> PhysicsWorld::body_pose(Entity e) const {
    const Impl& d = *impl_;
    if (const BodyRecord* rec = d.body_record(e)) {
        JPH::RVec3 p;
        JPH::Quat  q;
        d.system->GetBodyInterfaceNoLock().GetPositionAndRotation(rec->id, p, q);
        return Transform{ to_ae(p), to_ae(q), rec->scale };
    }
    if (d.characters_.contains(e)) {
        const WorldPose wp = d.world_pose_of(e);
        return Transform{ to_ae(d.characters_.get(e).character->GetPosition()), wp.rotation, wp.scale };
    }
    return std::nullopt;
}

Vec3 PhysicsWorld::linear_velocity(Entity e) const {
    if (const BodyRecord* rec = impl_->body_record(e))
        return to_ae(impl_->bodies().GetLinearVelocity(rec->id));
    if (impl_->characters_.contains(e))
        return to_ae(impl_->characters_.get(e).character->GetLinearVelocity());
    return Vec3(0.0f);
}

Vec3 PhysicsWorld::angular_velocity(Entity e) const {
    if (const BodyRecord* rec = impl_->body_record(e))
        return to_ae(impl_->bodies().GetAngularVelocity(rec->id));
    return Vec3(0.0f);
}

void PhysicsWorld::set_linear_velocity(Entity e, const Vec3& v) {
    if (const BodyRecord* rec = impl_->body_record(e)) {
        if (rec->motion == MotionType::Dynamic)
            impl_->bodies().SetLinearVelocity(rec->id, to_jolt(v)); // also wakes the body
    } else if (impl_->characters_.contains(e)) {
        impl_->characters_.get(e).character->SetLinearVelocity(to_jolt(v));
    }
}

void PhysicsWorld::set_angular_velocity(Entity e, const Vec3& w) {
    if (const BodyRecord* rec = impl_->body_record(e); rec && rec->motion == MotionType::Dynamic)
        impl_->bodies().SetAngularVelocity(rec->id, to_jolt(w));
}

void PhysicsWorld::add_force(Entity e, const Vec3& force) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) { bi.AddForce(id, to_jolt(force)); });
}

void PhysicsWorld::add_force_at_point(Entity e, const Vec3& force, const Vec3& world_point) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) {
        bi.AddForce(id, to_jolt(force), to_jolt_r(world_point));
    });
}

void PhysicsWorld::add_torque(Entity e, const Vec3& torque) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) { bi.AddTorque(id, to_jolt(torque)); });
}

void PhysicsWorld::add_impulse(Entity e, const Vec3& impulse) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) { bi.AddImpulse(id, to_jolt(impulse)); });
}

void PhysicsWorld::add_impulse_at_point(Entity e, const Vec3& impulse, const Vec3& world_point) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) {
        bi.AddImpulse(id, to_jolt(impulse), to_jolt_r(world_point));
    });
}

void PhysicsWorld::add_angular_impulse(Entity e, const Vec3& angular_impulse) {
    impl_->with_dynamic(e, [&](JPH::BodyInterface& bi, JPH::BodyID id) {
        bi.AddAngularImpulse(id, to_jolt(angular_impulse));
    });
}

Mat4 PhysicsWorld::interpolated_world_matrix(Entity e, f32 alpha) const {
    const Impl& d = *impl_;
    if (!d.reg.valid(e))
        return Mat4(1.0f);
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    if (const BodyRecord* rec = d.body_record(e); rec && rec->motion == MotionType::Dynamic) {
        const Vec3 p = glm::mix(rec->prev.position, rec->curr.position, alpha);
        const Quat q = glm::slerp(rec->prev.rotation, rec->curr.rotation, alpha);
        return compose(p, q, rec->scale);
    }
    if (d.characters_.contains(e)) {
        const CharacterRecord& rec = d.characters_.get(e);
        Mat4 m = d.world_matrix_of(e); // gameplay owns the character's rotation/scale
        m[3] = Vec4(glm::mix(rec.prev.position, rec.curr.position, alpha), 1.0f);
        return m;
    }
    return d.world_matrix_of(e);
}

std::span<const ContactEvent> PhysicsWorld::contact_events() const { return impl_->events; }

void PhysicsWorld::clear_contact_events() { impl_->events.clear(); }

void PhysicsWorld::drain_contact_events(std::vector<ContactEvent>& out) {
    out.insert(out.end(), impl_->events.begin(), impl_->events.end());
    impl_->events.clear();
}

} // namespace aether::physics
