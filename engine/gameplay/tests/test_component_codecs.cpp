// test_component_codecs.cpp — gameplay/physics component codecs through the scene serializer.
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/component_codecs.h"
#include "aether/gameplay/components.h"
#include "aether/physics/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::gameplay;
using namespace aether::physics;

namespace {
void roundtrip(const World& src, World& dst) {
    auto text = scene::save_scene_to_string(src);
    REQUIRE(text.has_value());
    REQUIRE(register_default_codecs(dst));
    auto result = scene::load_scene_from_string(dst, *text);
    REQUIRE(result.has_value());
    CHECK(result->warning_count == 0);
}
} // namespace

TEST_CASE("physics components round-trip") {
    World world;
    REQUIRE(register_default_codecs(world));
    REQUIRE(register_default_codecs(world)); // idempotent

    const Entity crate = world.create("Crate");
    RigidBodyComponent rb;
    rb.motion_type             = MotionType::Kinematic;
    rb.mass                    = 12.5f;
    rb.restitution             = 0.4f;
    rb.is_sensor               = true;
    rb.collision_layer         = 3;
    rb.locked_dofs             = dof::Plane2D;
    rb.initial_linear_velocity = Vec3(1, 2, 3);
    world.add<RigidBodyComponent>(crate, rb);
    ColliderComponent col = ColliderComponent::capsule(0.75f, 0.25f);
    col.local_offset      = Vec3(0, 1, 0);
    col.local_rotation    = glm::angleAxis(0.5f, Vec3(0, 0, 1));
    world.add<ColliderComponent>(crate, col);

    const Entity hull = world.create("Hull");
    world.add<ColliderComponent>(hull, ColliderComponent::convex_hull({ Vec3(0), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1) }));
    const Entity ground = world.create("Ground");
    world.add<ColliderComponent>(ground, ColliderComponent::triangle_mesh({ Vec3(0), Vec3(1, 0, 0), Vec3(0, 0, 1) }, { 0, 2, 1 }));

    const Entity hero = world.create("Hero");
    CharacterControllerComponent cc;
    cc.radius           = 0.4f;
    cc.jump_speed       = 7.0f;
    cc.desired_velocity = Vec3(9, 9, 9); // runtime input: not saved
    world.add<CharacterControllerComponent>(hero, cc);

    const Entity door = world.create("Door");
    HingeConstraintComponent hinge;
    hinge.target         = ground;
    hinge.local_axis     = Vec3(0, 0, 1);
    hinge.limits_enabled = true;
    hinge.min_angle_deg  = -45.0f;
    world.add<HingeConstraintComponent>(door, hinge);
    DistanceConstraintComponent rope;
    rope.target       = hero;
    rope.max_distance = 3.0f;
    world.add<DistanceConstraintComponent>(door, rope);
    world.add<PointConstraintComponent>(hull, PointConstraintComponent{ kNullEntity, Vec3(1, 1, 1) });
    world.add<FixedConstraintComponent>(hero, FixedConstraintComponent{ crate });

    World out;
    roundtrip(world, out);
    const Entity crate2 = scene::find_by_name(out, "Crate");
    const auto&  rb2    = out.get<RigidBodyComponent>(crate2);
    CHECK(rb2.motion_type == MotionType::Kinematic);
    CHECK(rb2.mass == 12.5f);
    CHECK(rb2.restitution == 0.4f);
    CHECK(rb2.is_sensor);
    CHECK(rb2.collision_layer == 3);
    CHECK(rb2.locked_dofs == dof::Plane2D);
    CHECK(rb2.initial_linear_velocity == Vec3(1, 2, 3));
    const auto& col2 = out.get<ColliderComponent>(crate2);
    CHECK(col2.shape == ColliderShape::Capsule);
    CHECK(col2.half_height == 0.75f);
    CHECK(col2.radius == 0.25f);
    CHECK(col2.local_offset == Vec3(0, 1, 0));
    CHECK(glm::abs(glm::dot(col2.local_rotation, col.local_rotation)) == doctest::Approx(1.0f));

    CHECK(out.get<ColliderComponent>(scene::find_by_name(out, "Hull")).points.size() == 4);
    const auto& tri = out.get<ColliderComponent>(scene::find_by_name(out, "Ground"));
    CHECK(tri.vertices.size() == 3);
    CHECK(tri.indices == std::vector<u32>{ 0, 2, 1 });

    const Entity hero2 = scene::find_by_name(out, "Hero");
    const auto&  cc2   = out.get<CharacterControllerComponent>(hero2);
    CHECK(cc2.radius == 0.4f);
    CHECK(cc2.jump_speed == 7.0f);
    CHECK(cc2.desired_velocity == Vec3(0.0f));
    CHECK((out.get<FixedConstraintComponent>(hero2).target == scene::find_by_name(out, "Crate")));

    const Entity door2  = scene::find_by_name(out, "Door");
    const auto&  hinge2 = out.get<HingeConstraintComponent>(door2);
    CHECK((hinge2.target == scene::find_by_name(out, "Ground")));
    CHECK(hinge2.local_axis == Vec3(0, 0, 1));
    CHECK(hinge2.limits_enabled);
    CHECK(hinge2.min_angle_deg == -45.0f);
    CHECK((out.get<DistanceConstraintComponent>(door2).target == hero2));
    CHECK(out.get<DistanceConstraintComponent>(door2).max_distance == 3.0f);
    const auto& point2 = out.get<PointConstraintComponent>(scene::find_by_name(out, "Hull"));
    CHECK((point2.target == kNullEntity));
    CHECK(point2.local_anchor == Vec3(1, 1, 1));
}

TEST_CASE("camera and material override components round-trip") {
    World world;
    REQUIRE(register_default_codecs(world));
    const Entity target = world.create("Target");
    const Entity cam    = world.create("Cam");
    FlyCameraComponent fly;
    fly.move_speed          = 12.0f;
    fly.require_look_button = false;
    fly.yaw_deg             = 33.0f; // runtime state: re-derived
    fly.initialized         = true;
    world.add<FlyCameraComponent>(cam, fly);
    OrbitCameraComponent orbit;
    orbit.follow   = target;
    orbit.distance = 7.5f;
    orbit.target   = Vec3(1, 2, 3);
    world.add<OrbitCameraComponent>(cam, orbit);
    world.add<MaterialOverridesComponent>(cam, MaterialOverridesComponent{ { AssetId{ 1, 2 }, AssetId{}, AssetId{ 0xabc, 0xdef } } });

    World out;
    roundtrip(world, out);
    const Entity cam2 = scene::find_by_name(out, "Cam");
    const auto&  fly2 = out.get<FlyCameraComponent>(cam2);
    CHECK(fly2.move_speed == 12.0f);
    CHECK_FALSE(fly2.require_look_button);
    CHECK_FALSE(fly2.initialized);
    const auto& orbit2 = out.get<OrbitCameraComponent>(cam2);
    CHECK((orbit2.follow == scene::find_by_name(out, "Target")));
    CHECK(orbit2.distance == 7.5f);
    CHECK(orbit2.target == Vec3(1, 2, 3));
    const auto& mo = out.get<MaterialOverridesComponent>(cam2);
    REQUIRE(mo.materials.size() == 3);
    CHECK(mo.materials[0] == AssetId{ 1, 2 });
    CHECK_FALSE(mo.materials[1].is_valid());
    CHECK(mo.slot(2) == AssetId{ 0xabc, 0xdef });
    CHECK_FALSE(mo.slot(7).is_valid());
}

TEST_CASE("malformed component documents are skipped with a warning") {
    World world;
    REQUIRE(register_default_codecs(world));
    const std::string doc = R"({
        "format": "aether.scene", "version": 1, "kind": "scene",
        "entities": [
            { "uuid": "0000000000000001", "components": { "Name": { "name": "A" },
              "RigidBody": { "motion": "floating" },
              "Collider": { "shape": "box", "half_extents": [1, 2, 3] } } },
            { "uuid": "0000000000000002", "components": { "Name": { "name": "B" },
              "Collider": { "shape": "box", "half_extents": [1, 2] },
              "MaterialOverrides": { "materials": ["nothex"] } } }
        ] })";
    auto result = scene::load_scene_from_string(world, doc);
    REQUIRE(result.has_value());
    CHECK(result->warning_count == 3);
    const Entity a = scene::find_by_name(world, "A");
    CHECK_FALSE(world.has<RigidBodyComponent>(a));
    CHECK(world.get<ColliderComponent>(a).half_extents == Vec3(1, 2, 3));
    CHECK_FALSE(world.has<ColliderComponent>(scene::find_by_name(world, "B")));
}
