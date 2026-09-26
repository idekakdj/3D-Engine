// test_components.cpp — ScriptComponent helpers, the "Script" scene codec and the subsystem.
#include "script_test_utils.h"

#include "aether/core/input.h"
#include "aether/core/subsystem.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scripting/lua_integration.h"
#include "aether/scripting/scripting_subsystem.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::scripting;
using aether::scripting::test::ScriptFixture;

TEST_CASE("ScriptComponent property helpers") {
    ScriptComponent c;
    c.set_property("speed", 1.0);
    c.set_property("speed", 2.0);
    c.set_property("name", std::string("x"));
    REQUIRE(c.properties.size() == 2);
    CHECK(std::get<f64>(*c.find_property("speed")) == 2.0);
    CHECK(c.find_property("missing") == nullptr);
    CHECK(c.remove_property("speed"));
    CHECK_FALSE(c.remove_property("speed"));
    CHECK(c.properties.size() == 1);
}

TEST_CASE("Script codec round-trips every value type") {
    World world;
    REQUIRE(register_script_component_codec(world));
    const Entity target = world.create("Target");
    const Entity e      = world.create("Scripted");
    ScriptComponent c;
    c.script  = "ai/patrol.lua";
    c.enabled = false;
    c.set_property("nil", std::monostate{});
    c.set_property("flag", true);
    c.set_property("count", i64{ -7 });
    c.set_property("speed", 2.5);
    c.set_property("whole_float", 3.0);
    c.set_property("label", std::string("hello \"world\""));
    c.set_property("v2", Vec2(1, 2));
    c.set_property("v3", Vec3(1.5f, -2, 0.1f));
    c.set_property("v4", Vec4(1, 2, 3, 4));
    c.set_property("q", Quat(0.5f, 0.5f, 0.5f, 0.5f));
    c.set_property("target", target);
    c.set_property("nobody", Entity{ kNullEntity });
    world.add<ScriptComponent>(e, c);

    auto text = scene::save_scene_to_string(world);
    REQUIRE(text);
    CHECK(text->find("\"Script\"") != std::string::npos);

    World loaded;
    REQUIRE(register_script_component_codec(loaded));
    auto result = scene::load_scene_from_string(loaded, *text);
    REQUIRE(result);
    CHECK(result->warning_count == 0);
    const Entity le = scene::find_by_name(loaded, "Scripted");
    const Entity lt = scene::find_by_name(loaded, "Target");
    REQUIRE(loaded.has<ScriptComponent>(le));
    const ScriptComponent& lc = loaded.get<ScriptComponent>(le);
    CHECK(lc.script == "ai/patrol.lua");
    CHECK_FALSE(lc.enabled);
    CHECK(lc.runtime_id == kInvalidU32);
    REQUIRE(lc.properties.size() == c.properties.size());
    for (usize i = 0; i < c.properties.size(); ++i) {
        CHECK(lc.properties[i].name == c.properties[i].name); // order preserved
    }
    CHECK(std::holds_alternative<std::monostate>(*lc.find_property("nil")));
    CHECK(std::get<bool>(*lc.find_property("flag")));
    CHECK(std::get<i64>(*lc.find_property("count")) == -7);
    CHECK(std::get<f64>(*lc.find_property("speed")) == 2.5);
    CHECK(std::get<f64>(*lc.find_property("whole_float")) == 3.0); // stays a float
    CHECK(std::get<std::string>(*lc.find_property("label")) == "hello \"world\"");
    CHECK(std::get<Vec2>(*lc.find_property("v2")) == Vec2(1, 2));
    CHECK(std::get<Vec3>(*lc.find_property("v3")) == Vec3(1.5f, -2, 0.1f));
    CHECK(std::get<Vec4>(*lc.find_property("v4")) == Vec4(1, 2, 3, 4));
    CHECK(std::get<Quat>(*lc.find_property("q")) == Quat(0.5f, 0.5f, 0.5f, 0.5f));
    CHECK(std::get<Entity>(*lc.find_property("target")) == lt); // resolved through the uuid
    CHECK((std::get<Entity>(*lc.find_property("nobody")) == kNullEntity));
}

TEST_CASE("Script codec skips malformed properties and rejects malformed components") {
    World world;
    REQUIRE(register_script_component_codec(world));
    const std::string doc = R"({
        "format": "aether.scene", "version": 1, "kind": "scene",
        "entities": [
            { "uuid": "0000000000000001", "components": { "Name": { "name": "Good" },
              "Script": { "script": "a.lua", "properties": [
                  { "name": "ok", "value": 1 },
                  { "name": "bad", "value": { "vec3": [1, 2] } },
                  { "value": 3 } ] } } },
            { "uuid": "0000000000000002", "components": { "Name": { "name": "Bad" },
              "Script": { "script": 42 } } }
        ] })";
    auto result = scene::load_scene_from_string(world, doc);
    REQUIRE(result);
    CHECK(result->warning_count == 1); // the malformed component
    const Entity good = scene::find_by_name(world, "Good");
    REQUIRE(world.has<ScriptComponent>(good));
    CHECK(world.get<ScriptComponent>(good).properties.size() == 1);
    CHECK_FALSE(world.has<ScriptComponent>(scene::find_by_name(world, "Bad")));
}

TEST_CASE("loading a scene into a world with a live VM attaches its scripts") {
    ScriptFixture f;
    f.write("hello.lua", "properties = { greeting = 'hi' }\nfunction on_start(self) self.said = self.greeting end");
    ScriptVM& vm = f.start(); // registers the codec on the world
    const std::string doc = R"({
        "format": "aether.scene", "version": 1, "kind": "scene",
        "entities": [ { "uuid": "00000000000000aa", "components": {
            "Name": { "name": "Greeter" },
            "Script": { "script": "hello.lua", "properties": [ { "name": "greeting", "value": "hey" } ] } } } ] })";
    REQUIRE(scene::load_scene_from_string(f.world, doc));
    vm.update(0.016f);
    const Entity e = scene::find_by_name(f.world, "Greeter");
    CHECK(std::get<std::string>(*vm.get_field(e, "said")) == "hey");

    // Clone (editor Duplicate): the copy gets its own instance.
    const Entity copy = scene::clone_entity(f.world, e);
    vm.update(0.016f);
    CHECK(vm.instance_count() == 2);
    CHECK(vm.instance_state(copy) == ScriptInstanceState::Running);
}

namespace {
struct CountingRegistrar {
    int* count;
    void operator()(ScriptVM& vm) const {
        ++*count;
        (void)lua_state(vm); // reachable
    }
};
} // namespace

TEST_CASE("ScriptingSubsystem follows the context world and runs registrars") {
    ScriptFixture f;
    f.write("sub.lua", "function on_update(self) self.n = (self.n or 0) + 1 end\nfunction on_fixed_update(self) self.f = true end");
    ScriptVMConfig cfg;
    cfg.script_root = f.root;
    cfg.hot_reload  = false;
    ScriptingSubsystem sub(cfg);
    int                registrar_runs = 0;
    sub.add_binding_registrar(CountingRegistrar{ &registrar_runs });
    CHECK(sub.vm() == nullptr);

    EngineContext engine;
    sub.on_startup(engine);
    CHECK(sub.vm() == nullptr); // no world yet

    engine.world = &f.world;
    const Entity e = f.spawn("sub.lua");
    FrameContext frame;
    frame.engine     = &engine;
    frame.time.delta = 0.016f;
    sub.on_update(frame);
    REQUIRE(sub.vm() != nullptr);
    CHECK(registrar_runs == 1);
    FixedContext fixed;
    fixed.engine = &engine;
    sub.on_fixed_update(fixed);
    sub.on_update(frame);
    CHECK(std::get<i64>(*sub.vm()->get_field(e, "n")) == 2);
    CHECK(std::get<bool>(*sub.vm()->get_field(e, "f")));

    int late = 0;
    sub.add_binding_registrar(CountingRegistrar{ &late });
    CHECK(late == 1); // runs for the existing VM immediately

    // Swapping worlds rebuilds the VM (and reruns registrars).
    World other;
    engine.world = &other;
    sub.on_update(frame);
    CHECK(registrar_runs == 2);
    CHECK(&sub.vm()->world() == &other);

    sub.on_shutdown();
    CHECK(sub.vm() == nullptr);
}

#ifdef AE_TEST_SCRIPT_DIR
TEST_CASE("the shipped sample scripts run without errors") {
    World          world;
    ScriptVMConfig cfg;
    cfg.script_root = AE_TEST_SCRIPT_DIR;
    cfg.hot_reload  = false;
    ScriptVM vm(world, cfg);

    InputState in{};
    in.keys[static_cast<usize>(Key::W)]         = ButtonState::Held;
    in.keys[static_cast<usize>(Key::LeftShift)] = ButtonState::Held;
    vm.set_input(&in);

    std::vector<Entity> entities;
    for (const char* script : { "rotator.lua", "bobber.lua", "spawner.lua", "player_controller.lua" }) {
        const Entity    e = world.create(script);
        ScriptComponent c;
        c.script = script;
        world.add<ScriptComponent>(e, c);
        entities.push_back(e);
    }
    int spawned_events = 0;
    for (int frame = 0; frame < 300; ++frame) { // 5 seconds at 60 Hz
        vm.update(1.0f / 60.0f);
        vm.fixed_update(1.0f / 60.0f);
        if (frame == 0) {
            REQUIRE(vm.run_string("events.subscribe('marker_spawned', function(p) spawned = (spawned or 0) + 1 end)"));
        }
    }
    if (auto n = vm.get_global("spawned"); n && std::holds_alternative<i64>(*n)) {
        spawned_events = static_cast<int>(std::get<i64>(*n));
    }
    for (const Entity e : entities) {
        CHECK(vm.instance_state(e) == ScriptInstanceState::Running);
    }
    CHECK(vm.error_count() == 0);
    CHECK(spawned_events >= 3);
    // The player moved forward (-Z) and the rotator turned.
    CHECK(world.get<TransformComponent>(entities[3]).local.position.z < -40.0f); // 5 s * 5 m/s * 2 sprint
    CHECK(world.get<TransformComponent>(entities[0]).local.rotation != Quat(1, 0, 0, 0));
    // Markers older than their lifetime were destroyed.
    CHECK(scene::child_count_of(world, entities[2]) <= 4);
    vm.set_input(nullptr);
}
#endif
