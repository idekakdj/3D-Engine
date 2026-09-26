// physics_tests.cpp — aether.physics unit/integration tests (doctest).
#include <doctest/doctest.h>

#include "aether/core/job_system.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_subsystem.h"
#include "aether/physics/physics_world.h"
#include "aether/physics/runtime.h"
#include "aether/scene/components.h"
#include "aether/scene/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace aether;
using namespace aether::physics;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// Ground slab whose top surface is at y = top.
Entity make_ground(World& w, f32 top = 0.0f) {
    const Entity g = w.create("Ground");
    w.get<TransformComponent>(g).local.position = Vec3(0.0f, top - 0.5f, 0.0f);
    w.add<ColliderComponent>(g, ColliderComponent::box(Vec3(50.0f, 0.5f, 50.0f)));
    return g;
}

Entity make_body(World& w, const Vec3& position, ColliderComponent collider,
                 RigidBodyComponent rb = {}) {
    const Entity e = w.create("Body");
    w.get<TransformComponent>(e).local.position = position;
    w.add<ColliderComponent>(e, std::move(collider));
    w.add<RigidBodyComponent>(e, rb);
    return e;
}

void run(PhysicsWorld& pw, int steps) {
    for (int i = 0; i < steps; ++i)
        pw.step(kDt);
}

Vec3 position_of(World& w, Entity e) { return w.get<TransformComponent>(e).local.position; }

bool events_contain(std::span<const ContactEvent> events, ContactEventType type, Entity a,
                    Entity b) {
    return std::any_of(events.begin(), events.end(), [&](const ContactEvent& ev) {
        return ev.type == type && ((ev.a == a && ev.b == b) || (ev.a == b && ev.b == a));
    });
}

} // namespace

TEST_CASE("physics: collision layer matrix is symmetric") {
    CollisionLayerMatrix m;
    CHECK(m.collides(1, 2));
    m.set_collides(1, 2, false);
    CHECK_FALSE(m.collides(1, 2));
    CHECK_FALSE(m.collides(2, 1));
    CHECK(m.collides(1, 1));
    CHECK_FALSE(m.collides(16, 0)); // out of range never collides
    m.set_collides(2, 1, true);
    CHECK(m.collides(1, 2));
}

TEST_CASE("physics: runtime is refcounted") {
    const u32 base = runtime_ref_count();
    {
        RuntimeRef a;
        CHECK(runtime_ref_count() == base + 1);
        {
            RuntimeRef b;
            CHECK(runtime_ref_count() == base + 2);
        }
        CHECK(runtime_ref_count() == base + 1);
    }
    CHECK(runtime_ref_count() == base);
}

TEST_CASE("physics: falling box settles on static ground at rest height") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ground = make_ground(w, 0.0f);
    const Entity box = make_body(w, Vec3(0.0f, 5.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)));

    pw.sync();
    CHECK(pw.has_body(ground));
    CHECK(pw.has_body(box));
    CHECK(pw.motion_type(ground) == MotionType::Static); // collider without rigid body => static
    CHECK(pw.motion_type(box) == MotionType::Dynamic);

    run(pw, 300); // 5 s
    const Vec3 p = position_of(w, box);
    CHECK(std::abs(p.y - 0.5f) < 0.01f);
    CHECK(std::abs(p.x) < 0.01f);
    CHECK(std::abs(p.z) < 0.01f);
    CHECK(w.get<TransformComponent>(box).dirty);
    CHECK(glm::length(pw.linear_velocity(box)) < 0.05f);
    CHECK(jolt_assert_failure_count() == 0);
}

TEST_CASE("physics: restitution makes a sphere bounce, zero restitution does not") {
    World        w;
    PhysicsWorld pw(w);
    make_ground(w, 0.0f);

    RigidBodyComponent bouncy;
    bouncy.restitution = 0.8f;
    bouncy.linear_damping = 0.0f;
    const Entity ball = make_body(w, Vec3(0.0f, 5.5f, 0.0f), ColliderComponent::sphere(0.5f), bouncy);
    RigidBodyComponent dead;
    dead.restitution = 0.0f;
    const Entity brick = make_body(w, Vec3(5.0f, 5.5f, 0.0f), ColliderComponent::sphere(0.5f), dead);

    bool  ball_hit = false, brick_hit = false;
    f32   ball_apex = 0.0f, brick_apex = 0.0f;
    for (int i = 0; i < 180; ++i) {
        pw.step(kDt);
        const f32 by = position_of(w, ball).y - 0.5f; // height of the sphere's bottom
        const f32 ry = position_of(w, brick).y - 0.5f;
        if (by < 0.05f)
            ball_hit = true;
        if (ry < 0.05f)
            brick_hit = true;
        if (ball_hit)
            ball_apex = std::max(ball_apex, by);
        if (brick_hit)
            brick_apex = std::max(brick_apex, ry);
    }
    CHECK(ball_hit);
    CHECK(brick_hit);
    CHECK(ball_apex > 2.0f); // ideal 0.8^2 * 5 m = 3.2 m
    CHECK(brick_apex < 0.1f);
}

TEST_CASE("physics: raycasts report entity, normal and fraction") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ground = make_ground(w, 0.0f);
    RigidBodyComponent fixed;
    fixed.motion_type = MotionType::Static;
    fixed.collision_layer = 3;
    const Entity pillar = make_body(w, Vec3(0.0f, 2.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)), fixed);
    pw.sync();

    // Straight down onto the pillar top (y = 2.5).
    auto hit = pw.raycast(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == pillar);
    CHECK(hit->fraction == doctest::Approx(7.5f / 20.0f).epsilon(1e-4));
    CHECK(hit->distance == doctest::Approx(7.5f).epsilon(1e-4));
    CHECK(hit->point.y == doctest::Approx(2.5f).epsilon(1e-4));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1e-4));

    // Non-normalised direction; misses the pillar and hits the ground.
    hit = pw.raycast(Vec3(3.0f, 10.0f, 0.0f), Vec3(0.0f, -7.0f, 0.0f), 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    CHECK(hit->fraction == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1e-4));

    // Too short.
    CHECK_FALSE(pw.raycast(Vec3(3.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 5.0f).has_value());

    // All hits, sorted by distance.
    const auto all = pw.raycast_all(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(all.size() == 2);
    CHECK(all[0].entity == pillar);
    CHECK(all[1].entity == ground);
    CHECK(all[0].fraction < all[1].fraction);

    // Filters: ignore entity, layer mask, static exclusion.
    QueryFilter ignore;
    ignore.ignore_entity = pillar;
    hit = pw.raycast(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f, ignore);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    QueryFilter layer_only;
    layer_only.layer_mask = 1u << 3;
    hit = pw.raycast(Vec3(3.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f, layer_only);
    CHECK_FALSE(hit.has_value());
    QueryFilter moving_only;
    moving_only.include_static = false;
    CHECK_FALSE(pw.raycast(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f, moving_only).has_value());
}

TEST_CASE("physics: sweeps and overlaps") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ground = make_ground(w, 0.0f);
    pw.sync();

    // Sphere of radius 0.5 swept down from y = 10 touches the ground when its centre is at 0.5.
    auto hit = pw.sweep_sphere(0.5f, Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    CHECK(hit->distance == doctest::Approx(9.5f).epsilon(2e-3));
    CHECK(hit->fraction == doctest::Approx(9.5f / 20.0f).epsilon(2e-3));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(hit->point.y == doctest::Approx(0.0f).scale(1.0f).epsilon(2e-3));

    hit = pw.sweep_box(Vec3(1.0f, 0.25f, 1.0f), Quat(1, 0, 0, 0), Vec3(2.0f, 5.0f, 0.0f),
                       Vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    CHECK(hit->distance == doctest::Approx(4.75f).epsilon(2e-3));

    hit = pw.sweep_capsule(0.5f, 0.25f, Quat(1, 0, 0, 0), Vec3(-2.0f, 5.0f, 0.0f),
                           Vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    CHECK(hit->distance == doctest::Approx(5.0f - 0.75f).epsilon(2e-3));

    // Sideways sweep above the ground misses.
    CHECK_FALSE(pw.sweep_sphere(0.5f, Vec3(0.0f, 3.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 10.0f).has_value());

    // Overlaps.
    auto overlaps = pw.overlap_sphere(Vec3(0.0f, 0.2f, 0.0f), 0.3f);
    REQUIRE(overlaps.size() == 1);
    CHECK(overlaps[0].entity == ground);
    CHECK(overlaps[0].penetration_depth > 0.05f);
    CHECK(pw.overlap_sphere(Vec3(0.0f, 2.0f, 0.0f), 0.3f).empty());
    CHECK(pw.overlap_box(Vec3(0.0f, 5.0f, 0.0f), Vec3(1.0f), Quat(1, 0, 0, 0)).empty());
    overlaps = pw.overlap_box(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f), Quat(1, 0, 0, 0));
    REQUIRE(overlaps.size() == 1);
    CHECK(overlaps[0].entity == ground);
}

TEST_CASE("physics: sensors emit trigger begin/end and don't collide") {
    World        w;
    PhysicsWorld pw(w);
    RigidBodyComponent trigger;
    trigger.motion_type = MotionType::Static;
    trigger.is_sensor = true;
    const Entity sensor = make_body(w, Vec3(0.0f, 5.0f, 0.0f), ColliderComponent::box(Vec3(2.0f, 0.5f, 2.0f)), trigger);
    const Entity ball = make_body(w, Vec3(0.0f, 10.0f, 0.0f), ColliderComponent::sphere(0.25f));

    std::vector<ContactEvent> events;
    for (int i = 0; i < 150; ++i) {
        pw.step(kDt);
        pw.drain_contact_events(events);
    }
    CHECK(position_of(w, ball).y < 0.0f); // fell straight through the trigger

    const auto begin = std::find_if(events.begin(), events.end(), [](const ContactEvent& ev) {
        return ev.type == ContactEventType::Begin;
    });
    REQUIRE(begin != events.end());
    CHECK(begin->is_trigger);
    CHECK(begin->a == sensor); // the sensor is always `a` in trigger events
    CHECK(begin->b == ball);
    const auto end = std::find_if(events.begin(), events.end(), [](const ContactEvent& ev) {
        return ev.type == ContactEventType::End;
    });
    REQUIRE(end != events.end());
    CHECK(end > begin);
    CHECK(end->is_trigger);
    CHECK(end->a == sensor);
    CHECK(std::count_if(events.begin(), events.end(), [](const ContactEvent& ev) {
              return ev.type == ContactEventType::Persist;
          }) > 0);
    CHECK(pw.contact_events().empty()); // drained
}

TEST_CASE("physics: contact events for landing and destruction") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ground = make_ground(w, 0.0f);
    const Entity box = make_body(w, Vec3(0.0f, 1.5f, 0.0f), ColliderComponent::box(Vec3(0.5f)));

    std::vector<ContactEvent> events;
    int steps = 0;
    while (!events_contain(events, ContactEventType::Begin, ground, box) && steps++ < 120) {
        pw.step(kDt);
        pw.drain_contact_events(events);
    }
    REQUIRE(events_contain(events, ContactEventType::Begin, ground, box));
    const auto begin = std::find_if(events.begin(), events.end(), [](const ContactEvent& ev) {
        return ev.type == ContactEventType::Begin;
    });
    CHECK_FALSE(begin->is_trigger);
    CHECK(begin->approach_speed > 1.0f);
    // Normal points from a to b: up if the ground is `a`.
    CHECK((begin->a == ground ? begin->normal.y : -begin->normal.y) > 0.9f);

    run(pw, 3);
    events.clear();
    pw.clear_contact_events();
    w.destroy(box);
    CHECK_FALSE(pw.has_body(box));
    pw.step(kDt);
    CHECK(events_contain(pw.contact_events(), ContactEventType::End, ground, box));
}

TEST_CASE("physics: kinematic body pushes a dynamic body") {
    World        w;
    PhysicsWorld pw(w);
    make_ground(w, 0.0f);
    const Entity crate = make_body(w, Vec3(0.0f, 0.5f, 0.0f), ColliderComponent::box(Vec3(0.5f)));
    RigidBodyComponent kin;
    kin.motion_type = MotionType::Kinematic;
    const Entity pusher = make_body(w, Vec3(-3.0f, 0.5f, 0.0f), ColliderComponent::box(Vec3(0.5f, 0.45f, 0.5f)), kin);

    for (int i = 0; i < 120; ++i) { // 2 s at 2 m/s: pusher ends at x = +1
        w.get<TransformComponent>(pusher).local.position.x += 2.0f * kDt;
        pw.step(kDt);
    }
    // The kinematic body follows the ECS exactly and is not written back.
    CHECK(position_of(w, pusher).x == doctest::Approx(1.0f).epsilon(1e-3));
    REQUIRE(pw.body_pose(pusher).has_value());
    CHECK(pw.body_pose(pusher)->position.x == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(position_of(w, crate).x > 1.5f); // pushed ahead of the pusher's front face

    // Once the ECS stops moving it, the kinematic body stops too.
    const f32 x = pw.body_pose(pusher)->position.x;
    run(pw, 30);
    CHECK(pw.body_pose(pusher)->position.x == doctest::Approx(x).epsilon(1e-5));
}

TEST_CASE("physics: character controller stands, climbs a step, is blocked by a steep slope") {
    World        w;
    PhysicsWorld pw(w);
    make_ground(w, 0.0f);

    // 0.2 m high step for x in [2, 8].
    const Entity step_block = w.create("Step");
    w.get<TransformComponent>(step_block).local.position = Vec3(5.0f, 0.1f, 0.0f);
    w.add<ColliderComponent>(step_block, ColliderComponent::box(Vec3(3.0f, 0.1f, 3.0f)));

    // 60 degree ramp facing +X, starting around x = -2.4 (steeper than the 45 degree limit).
    const Entity ramp = w.create("Ramp");
    auto& ramp_tc = w.get<TransformComponent>(ramp).local;
    ramp_tc.position = Vec3(-3.0f, 0.0f, 0.0f);
    ramp_tc.rotation = glm::angleAxis(glm::radians(-60.0f), Vec3(0.0f, 0.0f, 1.0f));
    w.add<ColliderComponent>(ramp, ColliderComponent::box(Vec3(3.0f, 0.5f, 3.0f)));

    const Entity player = w.create("Player");
    w.get<TransformComponent>(player).local.position = Vec3(0.0f, 0.0f, 0.0f);
    CharacterControllerComponent cc;
    cc.max_slope_deg = 45.0f;
    cc.step_up_height = 0.35f;
    w.add<CharacterControllerComponent>(player, cc);

    pw.sync();
    CHECK(pw.has_character(player));
    CHECK_FALSE(pw.has_body(player));

    // Stands still on flat ground.
    run(pw, 60);
    {
        const auto& out = w.get<CharacterControllerComponent>(player);
        CHECK(out.on_ground);
        CHECK(out.ground_state == GroundState::OnGround);
        CHECK(out.ground_normal.y > 0.99f);
        CHECK(std::abs(position_of(w, player).y) < 0.05f);
        CHECK(std::abs(position_of(w, player).x) < 0.01f);
    }

    // Walks +X onto the 0.2 m step.
    w.get<CharacterControllerComponent>(player).desired_velocity = Vec3(2.0f, 0.0f, 0.0f);
    run(pw, 120);
    {
        const Vec3 p = position_of(w, player);
        CHECK(p.x > 2.5f);
        CHECK(std::abs(p.y - 0.2f) < 0.05f);
        CHECK(w.get<CharacterControllerComponent>(player).on_ground);
        CHECK(w.get<CharacterControllerComponent>(player).velocity.x > 1.0f);
    }

    // Teleport back (ECS edit) and walk -X into the steep ramp.
    w.get<TransformComponent>(player).local.position = Vec3(0.0f, 0.0f, 0.0f);
    w.get<CharacterControllerComponent>(player).desired_velocity = Vec3(-2.0f, 0.0f, 0.0f);
    run(pw, 180);
    {
        const Vec3 p = position_of(w, player);
        CHECK(p.x > -3.0f); // blocked near the ramp's foot (x ~ -2.4 minus radius)
        CHECK(p.y < 0.5f);  // did not climb it
    }
    CHECK(jolt_assert_failure_count() == 0);
}

TEST_CASE("physics: character jumps") {
    World        w;
    PhysicsWorld pw(w);
    make_ground(w, 0.0f);
    const Entity player = w.create("Player");
    w.add<CharacterControllerComponent>(player, CharacterControllerComponent{});
    run(pw, 30);
    REQUIRE(w.get<CharacterControllerComponent>(player).on_ground);
    w.get<CharacterControllerComponent>(player).jump_requested = true;
    f32 apex = 0.0f;
    for (int i = 0; i < 60; ++i) {
        pw.step(kDt);
        apex = std::max(apex, position_of(w, player).y);
    }
    CHECK_FALSE(w.get<CharacterControllerComponent>(player).jump_requested); // consumed
    CHECK(apex > 1.0f); // v^2 / 2g = 25 / 19.62 = 1.27 m
    run(pw, 60);
    CHECK(w.get<CharacterControllerComponent>(player).on_ground);
}

TEST_CASE("physics: body lifecycle follows the ECS") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ground = make_ground(w, 0.0f);
    const Entity box = make_body(w, Vec3(0.0f, 2.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)));
    pw.sync();
    REQUIRE(pw.body_count() == 2);
    REQUIRE(pw.raycast(Vec3(0, 10, 0), Vec3(0, -1, 0), 20.0f)->entity == box);

    // Removing the rigid body leaves an implicit static collider.
    w.remove<RigidBodyComponent>(box);
    CHECK_FALSE(pw.has_body(box)); // destroyed immediately...
    pw.sync();
    CHECK(pw.motion_type(box) == MotionType::Static); // ...and recreated as static

    // Replacing a component rebuilds the body with the new settings.
    RigidBodyComponent kin;
    kin.motion_type = MotionType::Kinematic;
    w.add<RigidBodyComponent>(box, kin);
    pw.sync();
    CHECK(pw.motion_type(box) == MotionType::Kinematic);

    // Removing the collider removes the body.
    w.remove<ColliderComponent>(box);
    pw.sync();
    CHECK_FALSE(pw.has_body(box));
    w.add<ColliderComponent>(box, ColliderComponent::sphere(0.5f));
    pw.sync();
    CHECK(pw.has_body(box));

    // Destroying the entity removes the body; queries no longer see it.
    w.destroy(box);
    CHECK_FALSE(pw.has_body(box));
    CHECK(pw.body_count() == 1);
    const auto hit = pw.raycast(Vec3(0, 10, 0), Vec3(0, -1, 0), 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ground);
    run(pw, 2);
    CHECK(pw.body_count() == 1);
}

TEST_CASE("physics: bodies created before the PhysicsWorld are picked up") {
    World w;
    make_ground(w, 0.0f);
    const Entity box = make_body(w, Vec3(0.0f, 3.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)));
    PhysicsWorld pw(w);
    pw.sync();
    CHECK(pw.body_count() == 2);
    run(pw, 180);
    CHECK(std::abs(position_of(w, box).y - 0.5f) < 0.01f);
}

TEST_CASE("physics: identical worlds are bit-identical (determinism)") {
    auto build = [](World& w) {
        make_ground(w, 0.0f);
        std::vector<Entity> out;
        for (int i = 0; i < 24; ++i) {
            const f32 x = static_cast<f32>(i % 4) * 1.05f - 1.5f;
            const f32 y = 1.0f + static_cast<f32>(i / 4) * 1.1f;
            const f32 z = static_cast<f32>(i % 3) * 0.3f;
            ColliderComponent c = (i % 3 == 0) ? ColliderComponent::sphere(0.5f)
                                : (i % 3 == 1) ? ColliderComponent::box(Vec3(0.5f))
                                               : ColliderComponent::capsule(0.3f, 0.3f);
            RigidBodyComponent rb;
            rb.initial_angular_velocity = Vec3(0.1f * static_cast<f32>(i), 0.0f, 0.2f);
            out.push_back(make_body(w, Vec3(x, y, z), std::move(c), rb));
        }
        return out;
    };
    auto snapshot = [](World& w, const std::vector<Entity>& es) {
        std::vector<Transform> t;
        for (const Entity e : es)
            t.push_back(w.get<TransformComponent>(e).local);
        return t;
    };

    PhysicsWorldSettings mt;
    mt.job_backend = PhysicsJobBackend::JoltThreadPool;
    mt.worker_threads = 4;
    PhysicsWorldSettings st;
    st.job_backend = PhysicsJobBackend::SingleThreaded;

    World wa, wb, wc;
    const auto ea = build(wa);
    const auto eb = build(wb);
    const auto ec = build(wc);
    PhysicsWorld pa(wa, mt), pb(wb, mt), pc(wc, st);
    for (int i = 0; i < 240; ++i) {
        pa.step(kDt);
        pb.step(kDt);
        pc.step(kDt);
    }
    const auto sa = snapshot(wa, ea);
    const auto sb = snapshot(wb, eb);
    const auto sc = snapshot(wc, ec);
    REQUIRE(sa.size() == sb.size());
    CHECK(std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(Transform)) == 0);
    // Jolt is deterministic independent of the thread count.
    CHECK(std::memcmp(sa.data(), sc.data(), sa.size() * sizeof(Transform)) == 0);
    // Sanity: things actually moved and settled somewhere below their start.
    CHECK(sa.back().position.y < 1.0f + 5.0f * 1.1f);
}

TEST_CASE("physics: parented dynamic body writes the correct local transform") {
    World        w;
    PhysicsWorld pw(w);
    make_ground(w, 0.0f);

    const Entity parent = w.create("Parent");
    auto& pt = w.get<TransformComponent>(parent).local;
    pt.position = Vec3(10.0f, 0.0f, 0.0f);
    pt.rotation = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f));
    pt.scale = Vec3(2.0f);

    const Entity child = w.create_child(parent, "Child");
    w.get<TransformComponent>(child).local.position = Vec3(0.0f, 5.0f, 0.0f); // world (10, 10, 0)
    w.add<ColliderComponent>(child, ColliderComponent::sphere(0.5f)); // world radius 1.0
    w.add<RigidBodyComponent>(child, RigidBodyComponent{});

    pw.sync();
    REQUIRE(pw.body_pose(child).has_value());
    CHECK(pw.body_pose(child)->position.y == doctest::Approx(10.0f).epsilon(1e-4));

    run(pw, 240);
    const auto pose = pw.body_pose(child);
    REQUIRE(pose.has_value());
    CHECK(std::abs(pose->position.y - 1.0f) < 0.02f); // rests on the ground with scaled radius
    CHECK(std::abs(pose->position.x - 10.0f) < 0.05f);

    // parent_world * child_local must reproduce the simulated world pose.
    const Mat4 parent_world = w.get<TransformComponent>(parent).local.to_matrix();
    const auto& local = w.get<TransformComponent>(child).local;
    const Vec3 recon = Vec3(parent_world * Vec4(local.position, 1.0f));
    CHECK(glm::length(recon - pose->position) < 1e-4f);
    const Quat recon_rot = pt.rotation * local.rotation;
    CHECK(std::abs(glm::dot(recon_rot, pose->rotation)) > 0.99999f);
    CHECK(local.scale.x == doctest::Approx(1.0f)); // scale untouched
    CHECK(w.get<TransformComponent>(child).dirty);

    // The scene's own transform system agrees.
    w.update_transforms();
    CHECK(glm::length(Vec3(w.world_matrix(child)[3]) - pose->position) < 1e-3f);

    // Moving the (non-physics) parent teleports the child with it.
    w.get<TransformComponent>(parent).local.position = Vec3(20.0f, 0.0f, 0.0f);
    pw.sync();
    CHECK(pw.body_pose(child)->position.x == doctest::Approx(20.0f).epsilon(1e-3));
}

TEST_CASE("physics: render interpolation blends previous and current poses") {
    World        w;
    PhysicsWorld pw(w);
    const Entity ball = make_body(w, Vec3(0.0f, 10.0f, 0.0f), ColliderComponent::sphere(0.5f));
    run(pw, 10);
    const f32 before = position_of(w, ball).y;
    pw.step(kDt);
    const f32 after = position_of(w, ball).y;
    REQUIRE(after < before);
    CHECK(pw.interpolated_world_matrix(ball, 0.0f)[3].y == doctest::Approx(before));
    CHECK(pw.interpolated_world_matrix(ball, 1.0f)[3].y == doctest::Approx(after));
    const f32 mid = pw.interpolated_world_matrix(ball, 0.5f)[3].y;
    CHECK(mid == doctest::Approx(0.5f * (before + after)));
    // Teleports reset interpolation (no smear across the jump).
    w.get<TransformComponent>(ball).local.position = Vec3(50.0f, 10.0f, 0.0f);
    pw.sync();
    CHECK(pw.interpolated_world_matrix(ball, 0.0f)[3].x == doctest::Approx(50.0f));
}

TEST_CASE("physics: collision layers filter contacts") {
    PhysicsWorldSettings s;
    s.layers.set_collides(1, 2, false);
    World        w;
    PhysicsWorld pw(w, s);
    make_ground(w, 0.0f);
    RigidBodyComponent l1;
    l1.collision_layer = 1;
    RigidBodyComponent l2;
    l2.collision_layer = 2;
    const Entity low = make_body(w, Vec3(0.0f, 0.5f, 0.0f), ColliderComponent::box(Vec3(0.5f)), l1);
    const Entity high = make_body(w, Vec3(0.0f, 2.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)), l2);
    run(pw, 120);
    CHECK(std::abs(position_of(w, low).y - 0.5f) < 0.02f);
    CHECK(std::abs(position_of(w, high).y - 0.5f) < 0.02f); // passed through `low`
    CHECK_FALSE(pw.layers_collide(1, 2));
}

TEST_CASE("physics: constraints") {
    World        w;
    PhysicsWorld pw(w);

    // Pendulum on a point constraint to the world.
    const Entity bob = make_body(w, Vec3(2.0f, 10.0f, 0.0f), ColliderComponent::sphere(0.25f));
    PointConstraintComponent pivot;
    pivot.local_anchor = Vec3(-2.0f, 0.0f, 0.0f); // world (0, 10, 0)
    w.add<PointConstraintComponent>(bob, pivot);

    // Fixed weld between two dynamic boxes: the pair falls together, keeping its offset.
    const Entity a = make_body(w, Vec3(10.0f, 10.0f, 0.0f), ColliderComponent::box(Vec3(0.25f)));
    const Entity b = make_body(w, Vec3(11.0f, 10.0f, 0.0f), ColliderComponent::box(Vec3(0.25f)));
    w.add<FixedConstraintComponent>(b, FixedConstraintComponent{ a });

    // Distance rope to the world with a hinge-free swing.
    const Entity weight = make_body(w, Vec3(-10.0f, 7.0f, 0.0f), ColliderComponent::sphere(0.25f));
    DistanceConstraintComponent rope;
    rope.target_local_anchor = Vec3(-10.0f, 10.0f, 0.0f); // world point (no target)
    rope.min_distance = 0.0f;
    rope.max_distance = 3.0f;
    w.add<DistanceConstraintComponent>(weight, rope);

    // Hinged door with limits.
    const Entity door = make_body(w, Vec3(20.5f, 10.0f, 0.0f), ColliderComponent::box(Vec3(0.5f, 1.0f, 0.05f)));
    HingeConstraintComponent hinge;
    hinge.local_anchor = Vec3(-0.5f, 0.0f, 0.0f);
    hinge.local_axis = Vec3(0.0f, 0.0f, 1.0f); // swings under gravity in the XY plane
    hinge.limits_enabled = true;
    hinge.min_angle_deg = -30.0f;
    hinge.max_angle_deg = 30.0f;
    w.add<HingeConstraintComponent>(door, hinge);

    pw.sync();
    CHECK(pw.constraint_count() == 4);

    std::vector<DebugLine> lines;
    pw.build_debug_lines(lines, DebugDrawFlags::Constraints);
    CHECK(lines.size() >= 4u * 3u);

    run(pw, 120);
    CHECK(glm::length(pw.body_pose(bob)->position - Vec3(0.0f, 10.0f, 0.0f)) == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(pw.body_pose(bob)->position.y < 9.5f); // it swung down
    CHECK(glm::length(pw.body_pose(b)->position - pw.body_pose(a)->position) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(pw.body_pose(a)->position.y < 5.0f); // the welded pair falls freely
    CHECK(glm::length(pw.body_pose(weight)->position - Vec3(-10.0f, 10.0f, 0.0f)) <= 3.05f);
    // The door's free end dropped, but at most 30 degrees.
    const Vec3 door_dir = glm::normalize(pw.body_pose(door)->position - Vec3(20.0f, 10.0f, 0.0f));
    const f32  angle = glm::degrees(std::acos(std::clamp(door_dir.x, -1.0f, 1.0f)));
    CHECK(angle > 20.0f);
    CHECK(angle < 33.0f);

    // Removing a component removes its constraint; destroying a body detaches the other side.
    w.remove<PointConstraintComponent>(bob);
    CHECK(pw.constraint_count() == 3);
    w.destroy(a);
    CHECK(pw.constraint_count() == 2);
    run(pw, 2);
    CHECK(jolt_assert_failure_count() == 0);
}

TEST_CASE("physics: shapes - convex hull, cylinder, triangle mesh, offset collider") {
    World        w;
    PhysicsWorld pw(w);

    // Static triangle-mesh floor (two triangles, top at y = 0).
    const Entity floor = w.create("MeshFloor");
    w.add<ColliderComponent>(floor, ColliderComponent::triangle_mesh(
        { Vec3(-20, 0, -20), Vec3(20, 0, -20), Vec3(20, 0, 20), Vec3(-20, 0, 20) },
        { 0, 2, 1, 0, 3, 2 }));

    const Entity hull = make_body(w, Vec3(0.0f, 3.0f, 0.0f), ColliderComponent::convex_hull({
        Vec3(-0.5f, 0, -0.5f), Vec3(0.5f, 0, -0.5f), Vec3(0.5f, 0, 0.5f), Vec3(-0.5f, 0, 0.5f),
        Vec3(-0.5f, 1, -0.5f), Vec3(0.5f, 1, -0.5f), Vec3(0.5f, 1, 0.5f), Vec3(-0.5f, 1, 0.5f) }));
    const Entity cyl = make_body(w, Vec3(3.0f, 3.0f, 0.0f), ColliderComponent::cylinder(0.5f, 0.3f));
    ColliderComponent offset = ColliderComponent::box(Vec3(0.25f));
    offset.local_offset = Vec3(0.0f, 1.0f, 0.0f); // collider hangs 1 m above the entity origin
    const Entity off = make_body(w, Vec3(-3.0f, 3.0f, 0.0f), offset);

    // A dynamic triangle mesh is rejected (created static).
    RigidBodyComponent dyn;
    const Entity bad_mesh = make_body(w, Vec3(8.0f, 3.0f, 0.0f), ColliderComponent::triangle_mesh(
        { Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1) }, { 0, 2, 1 }), dyn);
    // An invalid hull produces no body (logged).
    const Entity bad_hull = make_body(w, Vec3(12.0f, 3.0f, 0.0f), ColliderComponent::convex_hull({ Vec3(0.0f) }));

    pw.sync();
    CHECK(pw.motion_type(bad_mesh) == MotionType::Static);
    CHECK_FALSE(pw.has_body(bad_hull));

    run(pw, 240);
    CHECK(std::abs(position_of(w, hull).y) < 0.02f);        // hull base at the origin
    CHECK(std::abs(position_of(w, cyl).y - 0.5f) < 0.02f);  // upright cylinder
    CHECK(std::abs(position_of(w, off).y + 0.75f) < 0.02f); // origin 1 m below the box centre
    CHECK(pw.raycast(Vec3(15, 5, 15), Vec3(0, -1, 0), 10.0f)->entity == floor);

    std::vector<DebugLine> lines;
    pw.build_debug_lines(lines, DebugDrawFlags::All);
    CHECK(lines.size() > 50u);
}

TEST_CASE("physics: forces, impulses and velocities") {
    World        w;
    PhysicsWorldSettings s;
    s.gravity = Vec3(0.0f);
    PhysicsWorld pw(w, s);
    RigidBodyComponent rb;
    rb.mass = 2.0f;
    rb.linear_damping = 0.0f;
    rb.angular_damping = 0.0f;
    const Entity e = make_body(w, Vec3(0.0f), ColliderComponent::sphere(0.5f), rb);
    pw.sync();

    pw.add_impulse(e, Vec3(4.0f, 0.0f, 0.0f)); // dv = J / m = 2 m/s
    pw.step(kDt);
    CHECK(pw.linear_velocity(e).x == doctest::Approx(2.0f).epsilon(1e-3));

    pw.set_linear_velocity(e, Vec3(0.0f));
    for (int i = 0; i < 60; ++i) { // F = 2 N for 1 s => dv = 1 m/s
        pw.add_force(e, Vec3(0.0f, 2.0f, 0.0f));
        pw.step(kDt);
    }
    CHECK(pw.linear_velocity(e).y == doctest::Approx(1.0f).epsilon(1e-2));

    pw.set_angular_velocity(e, Vec3(0.0f, 3.0f, 0.0f));
    CHECK(pw.angular_velocity(e).y == doctest::Approx(3.0f).epsilon(1e-4));
    pw.add_torque(e, Vec3(0.0f, 1.0f, 0.0f));
    pw.step(kDt);
    CHECK(pw.angular_velocity(e).y > 3.0f);
}

TEST_CASE("physics: subsystem drives fixed steps and interpolation alpha") {
    World            w;
    make_ground(w, 0.0f);
    const Entity box = make_body(w, Vec3(0.0f, 2.0f, 0.0f), ColliderComponent::box(Vec3(0.5f)));

    const u32        refs = runtime_ref_count();
    PhysicsSubsystem sys;
    EngineContext    ctx;
    ctx.world = &w;
    sys.on_startup(ctx);
    CHECK(runtime_ref_count() == refs + 2); // subsystem + its PhysicsWorld
    REQUIRE(sys.physics_world() != nullptr);

    sys.set_debug_draw(DebugDrawFlags::Shapes);
    CHECK(sys.advance(1.0f / 30.0f + 0.001f) == 2);
    CHECK(sys.interpolation_alpha() >= 0.0f);
    CHECK(sys.interpolation_alpha() < 1.0f);
    CHECK_FALSE(sys.debug_lines().empty());
    CHECK(sys.physics_world()->step_count() == 2);

    // Application-style driving through the ISubsystem hooks.
    FrameContext frame;
    frame.engine = &ctx;
    frame.time.delta = 1.0f / 60.0f;
    frame.time.fixed_delta = 1.0f / 60.0f;
    FixedContext fixed;
    fixed.engine = &ctx;
    for (int i = 0; i < 120; ++i) {
        sys.on_begin_frame(frame);
        sys.on_fixed_update(fixed);
        sys.on_update(frame);
    }
    CHECK(std::abs(position_of(w, box).y - 0.5f) < 0.01f);

    sys.on_shutdown();
    CHECK(sys.physics_world() == nullptr);
    CHECK(runtime_ref_count() == refs);
}

TEST_CASE("physics: engine job system backend matches the single-threaded result") {
    JobSystem::initialize(3);
    auto run_scene = [](PhysicsJobBackend backend) {
        World w;
        PhysicsWorldSettings s;
        s.job_backend = backend;
        std::vector<Entity> es;
        {
            PhysicsWorld pw(w, s);
            make_ground(w, 0.0f);
            for (int i = 0; i < 16; ++i)
                es.push_back(make_body(w, Vec3(static_cast<f32>(i % 4) * 0.9f, 1.0f + static_cast<f32>(i) * 0.8f, 0.0f),
                                       ColliderComponent::box(Vec3(0.4f))));
            run(pw, 180);
        }
        std::vector<Transform> t;
        for (const Entity e : es)
            t.push_back(w.get<TransformComponent>(e).local);
        return t;
    };
    const auto engine = run_scene(PhysicsJobBackend::EngineJobSystem);
    const auto single = run_scene(PhysicsJobBackend::SingleThreaded);
    JobSystem::shutdown();
    REQUIRE(engine.size() == single.size());
    CHECK(std::memcmp(engine.data(), single.data(), engine.size() * sizeof(Transform)) == 0);
}
