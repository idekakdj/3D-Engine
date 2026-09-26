// test_bindings.cpp — the script-facing engine API (math, entity, world, input, time) and the
// lua_integration.h escape hatch.
#include "script_test_utils.h"

#include "aether/core/input.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scripting/lua_integration.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::scripting;
using aether::scripting::test::ScriptFixture;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps))); }
} // namespace

TEST_CASE("math: vectors") {
    ScriptFixture f;
    f.start();
    CHECK(f.eval_as<Vec3>("Vec3(1, 2, 3) + Vec3(1, 1, 1)") == Vec3(2, 3, 4));
    CHECK(f.eval_as<Vec3>("Vec3(1, 2, 3) * 2") == Vec3(2, 4, 6));
    CHECK(f.eval_as<Vec3>("2 * Vec3(1, 2, 3)") == Vec3(2, 4, 6));
    CHECK(f.eval_as<Vec3>("Vec3(2, 4, 6) / 2") == Vec3(1, 2, 3));
    CHECK(f.eval_as<Vec3>("-Vec3(1, 0, 0)") == Vec3(-1, 0, 0));
    CHECK(f.eval_as<Vec3>("Vec3(5)") == Vec3(5));
    CHECK(f.eval_as<Vec3>("Vec3.new()") == Vec3(0));
    CHECK(f.eval_as<Vec3>("Vec3.up()") == Vec3(0, 1, 0));
    CHECK(f.eval_as<Vec3>("Vec3(1, 0, 0):cross(Vec3(0, 1, 0))") == Vec3(0, 0, 1));
    CHECK(f.eval_as<f64>("Vec3(3, 4, 0):length()") == doctest::Approx(5.0));
    CHECK(f.eval_as<f64>("Vec3(1, 2, 3):dot(Vec3(1, 1, 1))") == doctest::Approx(6.0));
    CHECK(f.eval_as<Vec3>("Vec3(0, 0, 0):normalized()") == Vec3(0)); // no NaN
    CHECK(f.eval_as<Vec3>("Vec3(0, 0, 0):lerp(Vec3(2, 2, 2), 0.5)") == Vec3(1));
    CHECK(f.eval_as<bool>("Vec3(1, 2, 3) == Vec3(1, 2, 3)"));
    CHECK(f.eval_as<std::string>("tostring(Vec3(1, 2, 3))") == "Vec3(1, 2, 3)");
    CHECK(f.eval_as<f64>("(function() local v = Vec3(1, 2, 3); v.y = 9; return v.y end)()") == 9.0);
    CHECK(f.eval_as<Vec2>("Vec2(1, 2) + Vec2(3, 4)") == Vec2(4, 6));
    CHECK(f.eval_as<Vec4>("Vec4(Vec3(1, 2, 3), 4)") == Vec4(1, 2, 3, 4));

    // Vectors are mutable userdata: assignment aliases, Vec3(v) copies; getters return copies.
    CHECK(f.eval_as<f64>("(function() local a = Vec3(1); local b = a; b.x = 5; return a.x end)()") == 5.0);
    CHECK(f.eval_as<f64>("(function() local a = Vec3(1); local b = Vec3(a); b.x = 5; return a.x end)()") == 1.0);
    // Bad arguments are script errors, not crashes.
    CHECK_FALSE(f.vm->run_string("local v = Vec3(1, 2, 3) + 5"));
    CHECK_FALSE(f.vm->run_string("local v = Vec3('a')"));
}

TEST_CASE("math: quaternions and transforms") {
    ScriptFixture f;
    f.start();
    const Quat q = f.eval_as<Quat>("Quat.angle_axis(math.pi / 2, Vec3.up())");
    CHECK(near(q * Vec3(1, 0, 0), Vec3(0, 0, -1)));
    CHECK(near(f.eval_as<Vec3>("Quat.angle_axis(math.pi / 2, Vec3.up()) * Vec3(1, 0, 0)"), Vec3(0, 0, -1)));
    CHECK(f.eval_as<Quat>("Quat()") == Quat(1, 0, 0, 0));
    CHECK(f.eval_as<Quat>("Quat(0, 0, 0, 1)") == Quat(1, 0, 0, 0)); // x, y, z, w order
    CHECK(f.eval_as<f64>("Quat(0, 0, 0, 1).w") == 1.0);
    CHECK(near(f.eval_as<Vec3>("Quat.from_euler(0.1, 0.2, 0.3):euler()"), Vec3(0.1f, 0.2f, 0.3f)));
    const Quat identity = f.eval_as<Quat>("Quat.angle_axis(1, Vec3(1, 1, 0)) * Quat.angle_axis(1, Vec3(1, 1, 0)):inverse()");
    CHECK(std::abs(identity.w) == doctest::Approx(1.0f));
    CHECK(near(f.eval_as<Vec3>("Quat.look_rotation(Vec3(1, 0, 0)) * Vec3.forward()"), Vec3(1, 0, 0)));
    CHECK(near(f.eval_as<Vec3>("Transform(Vec3(1, 2, 3)):transform_point(Vec3(1, 1, 1))"), Vec3(2, 3, 4)));
    CHECK(f.eval_as<Vec3>("Transform().scale") == Vec3(1));
}

TEST_CASE("entity: identity, transform and hierarchy") {
    ScriptFixture f;
    ScriptVM&     vm     = f.start();
    const Entity  parent = f.world.create("Parent");
    scene::set_local_position(f.world, parent, Vec3(10, 0, 0));
    const Entity child = f.world.create_child(parent, "Child");

    f.write("probe.lua", R"(
        function on_start(self)
            local e = self.entity
            self.name = e:name()
            self.same = world.find("Child") == e
            self.as_key = ({ [e] = "yes" })[world.find("Child")]
            self.parent_name = e:parent():name()
            e:set_position(Vec3(1, 2, 3))
            self.world_pos = e:world_position()
            e:translate(Vec3(0, 0, 1))
            e:set_scale(2)
            e:set_name("Renamed")
            e:set_tag(9)
            self.child_count = #e:parent():children()
        end
    )");
    ScriptComponent c;
    c.script = "probe.lua";
    f.world.add<ScriptComponent>(child, c);
    vm.update(0.016f);
    REQUIRE(vm.instance_state(child) == ScriptInstanceState::Running);

    CHECK(std::get<std::string>(*vm.get_field(child, "name")) == "Child");
    CHECK(std::get<bool>(*vm.get_field(child, "same")));
    CHECK(std::get<std::string>(*vm.get_field(child, "as_key")) == "yes");
    CHECK(std::get<std::string>(*vm.get_field(child, "parent_name")) == "Parent");
    CHECK(near(std::get<Vec3>(*vm.get_field(child, "world_pos")), Vec3(11, 2, 3)));
    CHECK(std::get<i64>(*vm.get_field(child, "child_count")) == 1);

    const Transform t = scene::local_transform(f.world, child);
    CHECK(t.position == Vec3(1, 2, 4));
    CHECK(t.scale == Vec3(2));
    CHECK(f.world.get<NameComponent>(child).name == "Renamed");
    CHECK(f.world.get<TagComponent>(child).tag == 9);
}

TEST_CASE("entity: world-space helpers, reparenting and look_at") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    const Entity  a  = f.world.create("A");
    const Entity  b  = f.world.create("B");
    scene::set_local_position(f.world, a, Vec3(5, 0, 0));
    scene::set_local_position(f.world, b, Vec3(0, 0, -3));

    f.run(R"(
        local a, b = world.find("A"), world.find("B")
        ok_keep = b:set_parent(a)              -- keeps world transform by default
        cycle = a:set_parent(b)                -- rejected: a is b's parent
        b:set_world_position(Vec3(1, 1, 1))
        a:look_at(Vec3(5, 0, -10))
        fwd = a:forward()
        root_count = #world.roots()
        b:set_parent(nil)
    )");
    CHECK(std::get<bool>(*vm.get_global("ok_keep")));
    CHECK_FALSE(std::get<bool>(*vm.get_global("cycle")));
    CHECK(near(std::get<Vec3>(*vm.get_global("fwd")), Vec3(0, 0, -1)));
    CHECK(std::get<i64>(*vm.get_global("root_count")) == 1);
    CHECK(near(scene::world_position(f.world, b), Vec3(1, 1, 1)));
    CHECK((scene::parent_of(f.world, b) == kNullEntity));

    f.run("world.find('A'):look_at(Vec3(10, 0, 0))");
    CHECK(near(f.eval_as<Vec3>("world.find('A'):forward()"), Vec3(1, 0, 0)));
    CHECK(near(f.eval_as<Vec3>("world.find('A'):right()"), Vec3(0, 0, 1)));
}

TEST_CASE("entity: components, visibility and scripts") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    const Entity  e  = f.world.create("Lamp");
    f.write("other.lua", "properties = { power = 3 }\nfunction on_update(self) self.alive = true end");

    f.run(R"(
        local e = world.find("Lamp")
        has_light_before = e:light() ~= nil
        local l = e:add_light("spot")
        l.intensity = 4
        l.color = Vec3(1, 0.5, 0)
        l.range = 20
        kind = e:light().kind
        local cam = e:add_camera()
        cam.fov = 75
        e:set_visible(false)
        visible = e:visible()
        e:add_script("other.lua", { power = 7 })
    )");
    CHECK_FALSE(std::get<bool>(*vm.get_global("has_light_before")));
    CHECK(std::get<std::string>(*vm.get_global("kind")) == "spot");
    const LightComponent& light = f.world.get<LightComponent>(e);
    CHECK(light.kind == LightKind::Spot);
    CHECK(light.intensity == 4.0f);
    CHECK(light.range == 20.0f);
    CHECK(light.color == Vec3(1, 0.5f, 0));
    CHECK(f.world.get<CameraComponent>(e).fov_y_deg == 75.0f);
    CHECK_FALSE(scene::is_visible(f.world, e));
    CHECK_FALSE(std::get<bool>(*vm.get_global("visible")));

    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Running);
    CHECK(f.eval_as<i64>("world.find('Lamp'):script().power") == 7);
    CHECK(f.eval_as<bool>("world.find('Lamp'):script().alive"));

    CHECK_FALSE(vm.run_string("world.find('Lamp'):add_light('laser')"));
    f.run("world.find('Lamp'):remove_light()");
    CHECK_FALSE(f.world.has<LightComponent>(e));
    CHECK(std::holds_alternative<std::monostate>(f.eval("world.find('Lamp'):light()")));

    // A stale component reference raises instead of crashing.
    f.run("local e = world.find('Lamp'); stale = e:add_light(); e:remove_light()");
    CHECK_FALSE(vm.run_string("stale.intensity = 2"));
    f.run("world.find('Lamp'):remove_script()");
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::None);
}

TEST_CASE("world: spawn, find and deferred destroy") {
    ScriptFixture f;
    f.write("spawner.lua", R"(
        function on_start(self)
            self.spawned = world.spawn("Bullet")
            self.child = world.spawn("Tip", self.spawned)
            self.child:set_position(Vec3(0, 0, -1))
        end
        function on_update(self)
            if self.spawned:valid() and not self.asked then
                self.asked = true
                self.spawned:destroy()
                self.still_valid_this_frame = self.spawned:valid()
            end
        end
    )");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("spawner.lua");
    vm.update(0.016f);
    const Entity bullet = scene::find_by_name(f.world, "Bullet");
    const Entity tip    = scene::find_by_name(f.world, "Tip");
    CHECK((bullet == kNullEntity)); // destroyed at the end of the first update (deferred)
    CHECK((tip == kNullEntity));
    CHECK(std::get<bool>(*vm.get_field(e, "still_valid_this_frame")));
    CHECK_FALSE(f.eval_as<bool>("world.find('Scripted'):script().spawned:valid()"));
    // Methods on a destroyed entity raise.
    CHECK_FALSE(f.vm->run_string("world.find('Scripted'):script().spawned:name()"));

    const Entity target = f.world.create("Target");
    const std::string uuid = scene::uuid_to_string(scene::uuid_of(f.world, target));
    CHECK((f.eval_as<Entity>("world.find_uuid('" + uuid + "')") == target));
    CHECK((f.eval_as<Entity>("world.find_path('Target')") == target));
    CHECK(std::holds_alternative<std::monostate>(f.eval("world.find('Nobody')")));
    CHECK(f.eval_as<i64>("world.count()") == static_cast<i64>(f.world.entity_count()));
    f.run("world.destroy(world.find('Target'))");
    CHECK_FALSE(f.world.valid(target));
}

TEST_CASE("an instance destroying its own entity") {
    ScriptFixture f;
    f.write("suicide.lua", R"(
        function on_update(self) self.entity:destroy() end
        function on_destroy(self) goodbye = true end
    )");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("suicide.lua");
    const Entity c  = f.world.create_child(e, "Child");
    vm.update(0.016f);
    CHECK_FALSE(f.world.valid(e));
    CHECK_FALSE(f.world.valid(c));
    CHECK(vm.instance_count() == 0);
    CHECK(vm.error_count() == 0);
}

TEST_CASE("input reads the snapshot handed to the VM") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    CHECK_FALSE(f.eval_as<bool>("input.available()"));
    CHECK_FALSE(f.eval_as<bool>("input.key_down('w')"));

    InputState in{};
    in.keys[static_cast<usize>(Key::W)]          = ButtonState::Held;
    in.keys[static_cast<usize>(Key::Space)]      = ButtonState::Pressed;
    in.keys[static_cast<usize>(Key::RightShift)] = ButtonState::Held;
    in.keys[static_cast<usize>(Key::F5)]         = ButtonState::Released;
    in.mouse[static_cast<usize>(MouseButton::Left)] = ButtonState::Pressed;
    in.cursor       = Vec2(100, 50);
    in.cursor_delta = Vec2(2, -1);
    vm.set_input(&in);

    CHECK(f.eval_as<bool>("input.available()"));
    CHECK(f.eval_as<bool>("input.key_down('W')"));
    CHECK(f.eval_as<bool>("input.key_pressed('space')"));
    CHECK_FALSE(f.eval_as<bool>("input.key_pressed('w')"));
    CHECK(f.eval_as<bool>("input.key_down('shift')"));
    CHECK(f.eval_as<bool>("input.key_released('f5')"));
    CHECK(f.eval_as<bool>("input.mouse_pressed('left')"));
    CHECK(f.eval_as<bool>("input.mouse_down('left')"));
    CHECK_FALSE(f.eval_as<bool>("input.mouse_down('right')"));
    CHECK(f.eval_as<Vec2>("input.mouse_position()") == Vec2(100, 50));
    CHECK(f.eval_as<Vec2>("input.mouse_delta()") == Vec2(2, -1));
    CHECK(f.eval_as<f64>("input.axis('w', 's')") == 1.0);
    CHECK(f.eval_as<f64>("input.axis('s', 'w')") == -1.0);
    CHECK_FALSE(vm.run_string("input.key_down('hyperdrive')"));
    CHECK_FALSE(vm.run_string("input.mouse_down('thumb')"));
    vm.set_input(nullptr);
}

TEST_CASE("time is read-only and tracks updates") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    f.write("clock.lua", R"(
        function on_update(self, dt) self.dt, self.t, self.frame = time.dt, time.time, time.frame end
        function on_fixed_update(self, dt) self.fixed_dt = time.dt end
    )");
    const Entity e = f.spawn("clock.lua");
    vm.update(0.5f);
    vm.fixed_update(0.02f);
    vm.update(0.25f);
    CHECK(std::get<f64>(*vm.get_field(e, "dt")) == doctest::Approx(0.25));
    CHECK(std::get<f64>(*vm.get_field(e, "t")) == doctest::Approx(0.75));
    CHECK(std::get<i64>(*vm.get_field(e, "frame")) == 2);
    CHECK(std::get<f64>(*vm.get_field(e, "fixed_dt")) == doctest::Approx(0.02));
    CHECK_FALSE(vm.run_string("time.time = 0"));
    CHECK(f.eval_as<f64>("time.fixed_time") == doctest::Approx(0.02));
}

TEST_CASE("lua_integration: modules, globals and conversions") {
    ScriptFixture f;
    ScriptVM&     vm  = f.start();
    sol::state&   lua = lua_state(vm);

    sol::table physics = lua.create_table();
    physics.set_function("gravity", []() { return Vec3(0, -9.81f, 0); });
    physics.set_function("fail", [](sol::this_state s) { raise_script_error(s, "no physics here"); });
    register_module(vm, "physics", physics);
    set_script_global(vm, "GAME_NAME", sol::make_object(lua, "Aether"));

    CHECK(near(f.eval_as<Vec3>("physics.gravity()"), Vec3(0, -9.81f, 0)));
    CHECK(f.eval_as<std::string>("GAME_NAME") == "Aether");
    CHECK_FALSE(vm.run_string("physics.gravity = nil"));
    auto err = vm.run_string("physics.fail()");
    REQUIRE_FALSE(err);
    CHECK(err.error().message.find("no physics here") != std::string::npos);
    CHECK(err.error().message.find("console:1") != std::string::npos);

    const Entity e   = f.world.create("E");
    sol::object  obj = to_lua(vm, e);
    CHECK((entity_from_lua(obj) == e));
    CHECK(to_lua(vm, e) == obj); // canonical identity
    CHECK(from_lua(to_lua(vm, ScriptValue{ Vec3(1, 2, 3) })).value() == ScriptValue{ Vec3(1, 2, 3) });
    CHECK_FALSE(entity_from_lua(sol::make_object(lua, 5)).has_value());
    CHECK(script_globals(vm).get<sol::object>("physics").valid());
}
