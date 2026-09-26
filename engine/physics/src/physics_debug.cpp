// physics_debug.cpp — wireframe debug lines (shapes, contacts, characters, constraints).
//
// Lines are generated from the collider description + the body's current simulated pose (the
// same local placement/scale rules the shape builder uses), so what is drawn is what collides.
#include "physics_world_impl.h"

#include "aether/core/log.h"

#include <array>
#include <cmath>

namespace aether::physics {

using namespace detail;

namespace {

constexpr u32 kColorStatic = pack_rgba(150, 150, 150);
constexpr u32 kColorKinematic = pack_rgba(80, 140, 255);
constexpr u32 kColorDynamic = pack_rgba(60, 220, 90);
constexpr u32 kColorSleeping = pack_rgba(40, 110, 60);
constexpr u32 kColorSensor = pack_rgba(255, 220, 40);
constexpr u32 kColorCharacterGround = pack_rgba(40, 230, 230);
constexpr u32 kColorCharacterAir = pack_rgba(255, 150, 40);
constexpr u32 kColorContact = pack_rgba(255, 40, 40);
constexpr u32 kColorContactNormal = pack_rgba(255, 60, 255);
constexpr u32 kColorConstraint = pack_rgba(255, 140, 0);
constexpr u32 kColorVelocity = pack_rgba(230, 230, 60);

constexpr u32 kCircleSegments = 24;
constexpr usize kMaxMeshTriangles = 20000;

// Maps collider-local points to world space: world = body * scale * (offset + rotation * p).
struct LocalToWorld {
    Vec3 body_position{ 0.0f };
    Quat body_rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    Vec3 scale{ 1.0f };
    Vec3 offset{ 0.0f };
    Quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    [[nodiscard]] Vec3 operator()(const Vec3& p) const {
        return body_position + body_rotation * (scale * (offset + rotation * p));
    }
};

class LineSink {
public:
    LineSink(std::vector<DebugLine>& out, const LocalToWorld& xf, u32 color)
        : out_(out), xf_(xf), color_(color) {}

    void local(const Vec3& a, const Vec3& b) { out_.push_back({ xf_(a), xf_(b), color_ }); }

    // Circle in the plane spanned by unit axes u, v around `center`.
    void circle(const Vec3& center, const Vec3& u, const Vec3& v, f32 radius,
                f32 start = 0.0f, f32 sweep = kTwoPi) {
        const u32 segments = std::max(4u, static_cast<u32>(kCircleSegments * sweep / kTwoPi));
        Vec3 prev = center + radius * (std::cos(start) * u + std::sin(start) * v);
        for (u32 i = 1; i <= segments; ++i) {
            const f32  t = start + sweep * static_cast<f32>(i) / static_cast<f32>(segments);
            const Vec3 p = center + radius * (std::cos(t) * u + std::sin(t) * v);
            local(prev, p);
            prev = p;
        }
    }

    void box(const Vec3& he) {
        std::array<Vec3, 8> c;
        for (u32 i = 0; i < 8; ++i)
            c[i] = Vec3((i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z);
        static constexpr u8 kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
                                              { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
                                              { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : kEdges)
            local(c[e[0]], c[e[1]]);
    }

    void sphere(f32 r) {
        circle(Vec3(0.0f), Vec3(1, 0, 0), Vec3(0, 1, 0), r);
        circle(Vec3(0.0f), Vec3(0, 1, 0), Vec3(0, 0, 1), r);
        circle(Vec3(0.0f), Vec3(1, 0, 0), Vec3(0, 0, 1), r);
    }

    // Y-aligned capsule (half_height = half cylinder length), centred on the origin.
    void capsule(f32 half_height, f32 r, const Vec3& center = Vec3(0.0f)) {
        const Vec3 top = center + Vec3(0, half_height, 0);
        const Vec3 bottom = center - Vec3(0, half_height, 0);
        circle(top, Vec3(1, 0, 0), Vec3(0, 0, 1), r);
        circle(bottom, Vec3(1, 0, 0), Vec3(0, 0, 1), r);
        for (const Vec3& d : { Vec3(r, 0, 0), Vec3(-r, 0, 0), Vec3(0, 0, r), Vec3(0, 0, -r) })
            local(top + d, bottom + d);
        // Hemisphere arcs in the XY and ZY planes.
        circle(top, Vec3(1, 0, 0), Vec3(0, 1, 0), r, 0.0f, kPi);
        circle(top, Vec3(0, 0, 1), Vec3(0, 1, 0), r, 0.0f, kPi);
        circle(bottom, Vec3(1, 0, 0), Vec3(0, 1, 0), r, kPi, kPi);
        circle(bottom, Vec3(0, 0, 1), Vec3(0, 1, 0), r, kPi, kPi);
    }

    void cylinder(f32 half_height, f32 r) {
        const Vec3 top(0, half_height, 0);
        const Vec3 bottom(0, -half_height, 0);
        circle(top, Vec3(1, 0, 0), Vec3(0, 0, 1), r);
        circle(bottom, Vec3(1, 0, 0), Vec3(0, 0, 1), r);
        for (const Vec3& d : { Vec3(r, 0, 0), Vec3(-r, 0, 0), Vec3(0, 0, r), Vec3(0, 0, -r) })
            local(top + d, bottom + d);
    }

    void convex_hull(const JPH::Shape* base) {
        if (base == nullptr || base->GetSubType() != JPH::EShapeSubType::ConvexHull)
            return;
        const auto*     hull = static_cast<const JPH::ConvexHullShape*>(base);
        const Vec3      com = to_ae(hull->GetCenterOfMass()); // hull points are COM-relative
        std::array<JPH::uint, 256> indices{};
        for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f) {
            const JPH::uint n = hull->GetFaceVertices(f, static_cast<JPH::uint>(indices.size()),
                                                      indices.data());
            for (JPH::uint i = 0; i < n; ++i) {
                const Vec3 a = com + to_ae(hull->GetPoint(indices[i]));
                const Vec3 b = com + to_ae(hull->GetPoint(indices[(i + 1) % n]));
                local(a, b);
            }
        }
    }

    void mesh(const ColliderComponent& c) {
        const usize tri_count = std::min(c.indices.size() / 3, kMaxMeshTriangles);
        for (usize t = 0; t < tri_count; ++t) {
            const u32 i0 = c.indices[3 * t], i1 = c.indices[3 * t + 1], i2 = c.indices[3 * t + 2];
            if (i0 >= c.vertices.size() || i1 >= c.vertices.size() || i2 >= c.vertices.size())
                continue;
            local(c.vertices[i0], c.vertices[i1]);
            local(c.vertices[i1], c.vertices[i2]);
            local(c.vertices[i2], c.vertices[i0]);
        }
    }

private:
    std::vector<DebugLine>& out_;
    const LocalToWorld&     xf_;
    u32                     color_;
};

void cross(std::vector<DebugLine>& out, const Vec3& p, f32 size, u32 color) {
    out.push_back({ p - Vec3(size, 0, 0), p + Vec3(size, 0, 0), color });
    out.push_back({ p - Vec3(0, size, 0), p + Vec3(0, size, 0), color });
    out.push_back({ p - Vec3(0, 0, size), p + Vec3(0, 0, size), color });
}

} // namespace

void PhysicsWorld::build_debug_lines(std::vector<DebugLine>& out, DebugDrawFlags flags) const {
    const Impl&               d = *impl_;
    const JPH::BodyInterface& bi = d.system->GetBodyInterfaceNoLock();

    if (has_flag(flags, DebugDrawFlags::Shapes) || has_flag(flags, DebugDrawFlags::Velocities)) {
        for (auto [e, rec] : d.bodies_.each()) {
            JPH::RVec3 p;
            JPH::Quat  q;
            bi.GetPositionAndRotation(rec.id, p, q);
            const Vec3 position = to_ae(p);
            const bool awake = rec.motion != MotionType::Static && bi.IsActive(rec.id);

            if (has_flag(flags, DebugDrawFlags::Velocities) && awake &&
                rec.motion == MotionType::Dynamic) {
                const Vec3 v = to_ae(bi.GetLinearVelocity(rec.id));
                out.push_back({ position, position + v * 0.1f, kColorVelocity });
            }
            if (!has_flag(flags, DebugDrawFlags::Shapes))
                continue;
            const auto* collider = d.reg.try_get<ColliderComponent>(e);
            if (collider == nullptr)
                continue;

            u32 color = kColorStatic;
            if (rec.sensor)
                color = kColorSensor;
            else if (rec.motion == MotionType::Kinematic)
                color = kColorKinematic;
            else if (rec.motion == MotionType::Dynamic)
                color = awake ? kColorDynamic : kColorSleeping;

            LocalToWorld xf;
            xf.body_position = position;
            xf.body_rotation = to_ae(q);
            xf.scale = rec.scale;
            xf.offset = collider->local_offset;
            xf.rotation = glm::normalize(collider->local_rotation);
            LineSink sink(out, xf, color);
            switch (collider->shape) {
            case ColliderShape::Box:          sink.box(glm::max(collider->half_extents, Vec3(1.0e-3f))); break;
            case ColliderShape::Sphere:       sink.sphere(collider->radius); break;
            case ColliderShape::Capsule:      sink.capsule(collider->half_height, collider->radius); break;
            case ColliderShape::Cylinder:     sink.cylinder(collider->half_height, collider->radius); break;
            case ColliderShape::ConvexHull:   sink.convex_hull(rec.base_shape.GetPtr()); break;
            case ColliderShape::TriangleMesh: sink.mesh(*collider); break;
            }
        }
    }

    if (has_flag(flags, DebugDrawFlags::Characters)) {
        for (auto [e, rec] : d.characters_.each()) {
            const JPH::CharacterVirtual& ch = *rec.character;
            const bool   grounded = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            LocalToWorld xf;
            xf.body_position = to_ae(ch.GetPosition());
            LineSink sink(out, xf, grounded ? kColorCharacterGround : kColorCharacterAir);
            sink.capsule(rec.half_height, rec.radius, Vec3(0.0f, rec.half_height + rec.radius, 0.0f));
            if (ch.GetGroundState() != JPH::CharacterBase::EGroundState::InAir) {
                const Vec3 gp = to_ae(ch.GetGroundPosition());
                out.push_back({ gp, gp + 0.5f * to_ae(ch.GetGroundNormal()), kColorContactNormal });
            }
        }
    }

    if (has_flag(flags, DebugDrawFlags::Contacts)) {
        for (const DebugContact& c : d.debug_contacts) {
            cross(out, c.point, 0.05f, c.sensor ? kColorSensor : kColorContact);
            out.push_back({ c.point, c.point + 0.3f * c.normal, kColorContactNormal });
        }
    }

    if (has_flag(flags, DebugDrawFlags::Constraints)) {
        for (const auto& [key, rec] : d.constraints_) {
            if (rec.constraint == nullptr)
                continue;
            JPH::RVec3 p1;
            JPH::Quat  q1;
            bi.GetPositionAndRotation(rec.body1, p1, q1);
            const Vec3 b1 = to_ae(p1);
            const Vec3 a1 = b1 + to_ae(q1) * rec.anchor1_local;
            Vec3       a2 = rec.anchor2_local;
            Vec3       b2 = a2;
            if (!rec.body2.IsInvalid()) {
                JPH::RVec3 p2;
                JPH::Quat  q2;
                bi.GetPositionAndRotation(rec.body2, p2, q2);
                b2 = to_ae(p2);
                a2 = b2 + to_ae(q2) * rec.anchor2_local;
            }
            out.push_back({ b1, a1, kColorConstraint });
            out.push_back({ a1, a2, kColorConstraint });
            out.push_back({ a2, b2, kColorConstraint });
            cross(out, a1, 0.05f, kColorConstraint);
        }
    }
}

} // namespace aether::physics
