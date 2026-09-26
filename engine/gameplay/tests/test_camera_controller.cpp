// test_camera_controller.cpp — fly / orbit controllers and their subsystem.
#include "aether/gameplay/camera_controller.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::gameplay;

namespace {

bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) {
    return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps)));
}

InputMap camera_input(std::initializer_list<Key> keys, Vec2 mouse_delta = Vec2(0.0f), bool look = false,
                      f32 scroll = 0.0f, bool pan = false) {
    InputMap map;
    map.add_default_camera_bindings();
    InputState s{};
    for (const Key k : keys) {
        s.keys[static_cast<usize>(k)] = ButtonState::Held;
    }
    if (look) {
        s.mouse[static_cast<usize>(MouseButton::Right)] = ButtonState::Held;
    }
    if (pan) {
        s.mouse[static_cast<usize>(MouseButton::Middle)] = ButtonState::Held;
    }
    s.cursor_delta = mouse_delta;
    s.scroll       = Vec2(0.0f, scroll);
    map.update(s);
    return map;
}

} // namespace

TEST_CASE("yaw/pitch <-> rotation round trip") {
    for (const f32 yaw : { -170.0f, -90.0f, 0.0f, 45.0f, 135.0f }) {
        for (const f32 pitch : { -80.0f, -30.0f, 0.0f, 60.0f }) {
            f32 y = 0.0f;
            f32 p = 0.0f;
            yaw_pitch_from_rotation(rotation_from_yaw_pitch(yaw, pitch), y, p);
            CHECK(y == doctest::Approx(yaw).epsilon(1e-3));
            CHECK(p == doctest::Approx(pitch).epsilon(1e-3));
        }
    }
    // Yaw 0 looks down -Z; positive yaw turns left (towards -X); positive pitch looks up.
    CHECK(near(rotation_from_yaw_pitch(0, 0) * Vec3(0, 0, -1), Vec3(0, 0, -1)));
    CHECK(near(rotation_from_yaw_pitch(90, 0) * Vec3(0, 0, -1), Vec3(-1, 0, 0)));
    CHECK(near(rotation_from_yaw_pitch(0, 90) * Vec3(0, 0, -1), Vec3(0, 1, 0)));
}

TEST_CASE("fly camera: moves along its facing, boost, look only with the button") {
    FlyCameraComponent cam;
    Transform          t;
    update_fly_camera(cam, t, camera_input({ Key::W }), 1.0f);
    CHECK(cam.initialized);
    CHECK(near(t.position, Vec3(0, 0, -5)));

    update_fly_camera(cam, t, camera_input({ Key::W, Key::LeftShift }), 0.5f);
    CHECK(near(t.position, Vec3(0, 0, -15)));

    // Diagonal movement is normalised.
    Transform d;
    FlyCameraComponent cam2;
    update_fly_camera(cam2, d, camera_input({ Key::W, Key::D }), 1.0f);
    CHECK(glm::length(d.position) == doctest::Approx(5.0f));

    // Mouse delta without the look button does nothing...
    update_fly_camera(cam, t, camera_input({}, Vec2(100, 0)), 0.016f);
    CHECK(cam.yaw_deg == doctest::Approx(0.0f));
    // ...with it, moving right turns right (negative yaw).
    update_fly_camera(cam, t, camera_input({}, Vec2(100, 0), true), 0.016f);
    CHECK(cam.yaw_deg == doctest::Approx(-15.0f));
    // Pitch is clamped.
    update_fly_camera(cam, t, camera_input({}, Vec2(0, -10000), true), 0.016f);
    CHECK(cam.pitch_deg == doctest::Approx(89.0f));
    // The wheel scales speed while looking, within limits.
    update_fly_camera(cam, t, camera_input({}, Vec2(0), true, 1.0f), 0.016f);
    CHECK(cam.move_speed == doctest::Approx(6.0f));
    update_fly_camera(cam, t, camera_input({}, Vec2(0), true, 1000.0f), 0.016f);
    CHECK(cam.move_speed == doctest::Approx(cam.max_speed));
}

TEST_CASE("fly camera: initial orientation is taken from the transform") {
    FlyCameraComponent cam;
    Transform          t;
    t.rotation = rotation_from_yaw_pitch(30.0f, -10.0f);
    update_fly_camera(cam, t, camera_input({}), 0.016f);
    CHECK(cam.yaw_deg == doctest::Approx(30.0f));
    CHECK(cam.pitch_deg == doctest::Approx(-10.0f));
}

TEST_CASE("orbit camera: orbit, zoom, pan and follow") {
    OrbitCameraComponent cam;
    cam.pitch_deg = 0.0f;
    Transform t;
    update_orbit_camera(cam, t, camera_input({}), Vec3(0), false);
    CHECK(near(t.position, Vec3(0, 0, 10)));
    CHECK(near(t.rotation * Vec3(0, 0, -1), Vec3(0, 0, -1))); // looks at the target

    update_orbit_camera(cam, t, camera_input({}, Vec2(-360, 0), true), Vec3(0), false); // yaw +90
    CHECK(near(t.position, Vec3(10, 0, 0)));

    update_orbit_camera(cam, t, camera_input({}, Vec2(0), false, 1.0f), Vec3(0), false);
    CHECK(cam.distance == doctest::Approx(9.0f));
    update_orbit_camera(cam, t, camera_input({}, Vec2(0), false, -1000.0f), Vec3(0), false);
    CHECK(cam.distance == doctest::Approx(cam.max_distance));
    cam.distance = 10.0f;

    const Vec3 before = cam.target;
    update_orbit_camera(cam, t, camera_input({}, Vec2(50, 0), false, 0.0f, true), Vec3(0), false);
    CHECK(glm::length(cam.target - before) > 0.1f);

    update_orbit_camera(cam, t, camera_input({}), Vec3(3, 4, 5), true);
    CHECK(near(cam.target, Vec3(3, 4, 5)));
    CHECK(glm::distance(t.position, Vec3(3, 4, 5)) == doctest::Approx(10.0f));
}

TEST_CASE("CameraControllerSubsystem drives components in a world") {
    World        world;
    const Entity fly = world.create("Fly");
    world.add<CameraComponent>(fly);
    world.add<FlyCameraComponent>(fly);
    const Entity target = world.create("Target");
    scene::set_local_position(world, target, Vec3(0, 1, 0));
    const Entity orbit = world.create("Orbit");
    OrbitCameraComponent oc;
    oc.follow   = target;
    oc.distance = 4.0f;
    world.add<OrbitCameraComponent>(orbit, oc);

    CameraControllerSubsystem sub;
    sub.update_world(world, camera_input({ Key::W }), 1.0f);
    CHECK(near(scene::local_transform(world, fly).position, Vec3(0, 0, -5)));
    CHECK(glm::distance(scene::world_position(world, orbit), Vec3(0, 1, 0)) == doctest::Approx(4.0f));

    // Without a window the own map sees no input: nothing moves.
    EngineContext engine;
    engine.world = &world;
    sub.on_startup(engine);
    FrameContext frame;
    frame.engine     = &engine;
    frame.time.delta = 1.0f;
    sub.on_update(frame);
    CHECK(near(scene::local_transform(world, fly).position, Vec3(0, 0, -5)));
}
