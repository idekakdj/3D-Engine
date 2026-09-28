// test_visual_script.cpp — visual scripts (ADR-0018): graph editing rules, JSON round trip,
// compiler errors, and compiled graphs running in a ScriptVM (events, flow, variables as
// properties, input, timers, custom events, hot reload).
#include "script_test_utils.h"

#include "aether/core/input.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/transform_utils.h"
#include "aether/scripting/visual_script.h"

#include <doctest/doctest.h>

#include <string>

using namespace aether;
using namespace aether::scripting;
using aether::scripting::test::ScriptFixture;

namespace {
bool has_error(const GraphCompileResult& r, std::string_view text) {
    for (const GraphError& e : r.errors) {
        if (e.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("visual script: node library and connection rules") {
    CHECK(node_library().size() > 50);
    for (const NodeDef& d : node_library()) {
        CHECK(find_node_def(d.type) == &d); // unique ids
    }
    CHECK(find_node_def("nope") == nullptr);

    VisualGraph g;
    const u32   start = g.add_node("event.start");
    const u32   print = g.add_node("action.print");
    const u32   add = g.add_node("math.add");
    const u32   pos = g.add_node("data.get_position");
    CHECK(g.add_node("unknown.node") == 0);
    CHECK(std::get<f64>(g.find(add)->values.at("a")) == 0.0);

    CHECK(g.connect(start, "then", print, "in"));
    CHECK_FALSE(g.connect(start, "then", add, "a"));       // exec -> number
    CHECK_FALSE(g.connect(pos, "position", add, "a"));     // vector -> number
    CHECK(g.connect(pos, "position", print, "value"));     // anything -> Any
    CHECK(g.connect(add, "result", print, "value"));       // replaces the previous data wire
    CHECK(g.input_link(print, "value")->from_node == add);
    CHECK_FALSE(g.connect(add, "result", add, "a"));       // no self links
    CHECK_FALSE(g.connect(start, "missing", print, "in")); // unknown pin
    const u32 print2 = g.add_node("action.print");
    CHECK(g.connect(start, "then", print2, "in")); // an exec output drives one node
    CHECK(g.output_link(start, "then")->to_node == print2);
    CHECK(g.links.size() == 2);

    g.remove_node(add);
    CHECK(g.find(add) == nullptr);
    CHECK(g.input_link(print, "value") == nullptr);

    // Variables type Get / Set Variable pins.
    g.variables.push_back(GraphVariable{ "speed", PinType::Number, 2.0 });
    const u32 get = g.add_node("data.get_variable");
    CHECK(g.find(get)->variable == "speed");
    CHECK(g.pin_type(*g.find(get), "value", true) == PinType::Number);
    const u32 mul = g.add_node("math.multiply");
    CHECK(g.connect(get, "value", mul, "a"));
}

TEST_CASE("visual script: JSON round trip") {
    VisualGraph g = make_starter_graph();
    g.variables.push_back(GraphVariable{ "label", PinType::String, std::string("hi \"there\"") });
    g.variables.push_back(GraphVariable{ "dir", PinType::Vector, Vec3(1, 2, 3) });
    g.variables.push_back(GraphVariable{ "on", PinType::Bool, true });
    const u32 set = g.add_node("action.set_variable", Vec2(5.0f, 6.0f));
    g.find(set)->variable = "dir";
    const std::string text = graph_to_json(g);
    auto              back = graph_from_json(text);
    REQUIRE(back);
    CHECK(graph_to_json(*back) == text);
    CHECK(back->nodes.size() == g.nodes.size());
    CHECK(back->links.size() == g.links.size());
    CHECK(std::get<Vec3>(back->variables[1].value) == Vec3(1, 2, 3));
    CHECK(back->find(set)->variable == "dir");
    CHECK(back->find(set)->position == Vec2(5.0f, 6.0f));

    CHECK_FALSE(graph_from_json("not json"));
    CHECK_FALSE(graph_from_json(R"({"format":"aether.scene","version":1})"));
    CHECK_FALSE(graph_from_json(R"({"format":"aether.graph","version":2})"));
    CHECK_FALSE(graph_from_json(R"({"format":"aether.graph","version":1,"nodes":[{"type":"event.start"}]})"));
}

TEST_CASE("visual script: compiler errors") {
    VisualGraph g;
    const u32   start = g.add_node("event.start");
    const u32   a = g.add_node("action.print");
    const u32   b = g.add_node("action.print");
    g.connect(start, "then", a, "in");
    g.connect(a, "then", b, "in");
    g.links.push_back(GraphLink{ b, "then", a, "in" }); // a -> b -> a
    CHECK(has_error(compile_graph_to_lua(g), "loops back"));

    VisualGraph d;
    const u32   m1 = d.add_node("math.add");
    const u32   m2 = d.add_node("math.add");
    const u32   s = d.add_node("event.start");
    const u32   p = d.add_node("action.print");
    d.connect(s, "then", p, "in");
    d.connect(m1, "result", p, "value");
    d.links.push_back(GraphLink{ m2, "result", m1, "a" });
    d.links.push_back(GraphLink{ m1, "result", m2, "a" });
    CHECK(has_error(compile_graph_to_lua(d), "loop"));

    VisualGraph v;
    const u32   get = v.add_node("data.get_variable");
    (void)get;
    CHECK(has_error(compile_graph_to_lua(v), "no variable selected"));
    v.variables.push_back(GraphVariable{ "2bad", PinType::Number, 0.0 });
    CHECK(has_error(compile_graph_to_lua(v), "invalid variable name"));

    VisualGraph u;
    GraphNode   n;
    n.id = 1;
    n.type = "action.teleport";
    u.nodes.push_back(n);
    CHECK(has_error(compile_graph_to_lua(u), "unknown node type"));

    VisualGraph t; // a hand-edited wire with incompatible types
    const u32   pos = t.add_node("data.get_position");
    const u32   add = t.add_node("math.add");
    t.links.push_back(GraphLink{ pos, "position", add, "a" });
    CHECK(has_error(compile_graph_to_lua(t), "cannot take a vector"));

    CHECK(compile_graph_to_lua(make_starter_graph()).ok());
}

TEST_CASE("visual script: a compiled graph runs like a Lua script") {
    ScriptFixture f;
    // On Start: position = (0, 0, 0); On Update: Move By (speed x dt) along +X; Print once > 1 m.
    VisualGraph g;
    g.variables.push_back(GraphVariable{ "speed", PinType::Number, 2.0 });
    g.variables.push_back(GraphVariable{ "announced", PinType::Bool, false });
    const u32 start = g.add_node("event.start");
    const u32 setp = g.add_node("action.set_position");
    g.connect(start, "then", setp, "in");
    const u32 update = g.add_node("event.update");
    const u32 move = g.add_node("action.translate");
    g.connect(update, "then", move, "in");
    const u32 speed = g.add_node("data.get_variable");
    g.find(speed)->variable = "speed";
    const u32 dt = g.add_node("data.delta_time");
    const u32 mul = g.add_node("math.multiply");
    g.connect(speed, "value", mul, "a");
    g.connect(dt, "seconds", mul, "b");
    const u32 vec = g.add_node("vector.make");
    g.connect(mul, "result", vec, "x");
    g.connect(vec, "vector", move, "offset");
    // Branch: x > 1 and not announced -> set announced, emit "arrived".
    const u32 branch = g.add_node("flow.branch");
    g.connect(move, "then", branch, "in");
    const u32 pos = g.add_node("data.get_position");
    const u32 brk = g.add_node("vector.break");
    g.connect(pos, "position", brk, "vector");
    const u32 gt = g.add_node("compare.greater");
    g.connect(brk, "x", gt, "a");
    g.find(gt)->values["b"] = 1.0;
    const u32 announced = g.add_node("data.get_variable");
    g.find(announced)->variable = "announced";
    const u32 notn = g.add_node("logic.not");
    g.connect(announced, "value", notn, "a");
    const u32 andn = g.add_node("logic.and");
    g.connect(gt, "result", andn, "a");
    g.connect(notn, "result", andn, "b");
    g.connect(andn, "result", branch, "condition");
    const u32 seta = g.add_node("action.set_variable");
    g.find(seta)->variable = "announced";
    g.find(seta)->values["value"] = true;
    g.connect(branch, "true", seta, "in");
    const u32 emit = g.add_node("action.emit_event");
    g.find(emit)->values["name"] = std::string("arrived");
    g.connect(seta, "then", emit, "in");
    // Another entity reacts to the event.
    const GraphCompileResult compiled = compile_graph_to_lua(g, "mover.aegraph");
    INFO(compiled.lua);
    REQUIRE(compiled.ok());
    f.write("mover.aegraph", graph_to_json(g));

    VisualGraph listener;
    listener.variables.push_back(GraphVariable{ "heard", PinType::Number, 0.0 });
    const u32 on_event = listener.add_node("event.custom");
    listener.find(on_event)->values["name"] = std::string("arrived");
    const u32 inc = listener.add_node("action.set_variable");
    listener.find(inc)->variable = "heard";
    const u32 get = listener.add_node("data.get_variable");
    listener.find(get)->variable = "heard";
    const u32 plus = listener.add_node("math.add");
    listener.find(plus)->values["b"] = 1.0;
    listener.connect(get, "value", plus, "a");
    listener.connect(plus, "result", inc, "value");
    listener.connect(on_event, "then", inc, "in");
    f.write("listener.aegraph", graph_to_json(listener));

    ScriptVM& vm = f.start();
    auto      declared = vm.declared_properties("mover.aegraph");
    REQUIRE(declared);
    CHECK(declared->size() == 2); // the graph variables are the script's properties

    const Entity mover = f.world.create("Mover");
    ScriptComponent c;
    c.script = "mover.aegraph";
    c.set_property("speed", 4.0); // inspector override of a graph variable
    f.world.add<ScriptComponent>(mover, c);
    const Entity ear = f.spawn("listener.aegraph", "Ear");
    scene::set_local_position(f.world, mover, Vec3(5, 5, 5)); // On Start resets it

    vm.update(0.1f); // start + first update: x = 0.4
    CHECK(vm.error_count() == 0);
    CHECK(f.world.get<TransformComponent>(mover).local.position.x == doctest::Approx(0.4f));
    CHECK(f.world.get<TransformComponent>(mover).local.position.y == doctest::Approx(0.0f));
    for (int i = 0; i < 5; ++i) {
        vm.update(0.1f); // x = 2.4 after 6 updates
    }
    CHECK(f.world.get<TransformComponent>(mover).local.position.x == doctest::Approx(2.4f));
    CHECK(std::get<bool>(*vm.get_field(mover, "announced")));
    CHECK(std::get<f64>(*vm.get_field(ear, "heard")) == doctest::Approx(1.0)); // emitted once
    CHECK(vm.error_count() == 0);
}

TEST_CASE("visual script: input, sequence, delay, timers, spawn and hot reload") {
    ScriptFixture f;
    VisualGraph   g;
    g.variables.push_back(GraphVariable{ "count", PinType::Number, 0.0 });
    g.variables.push_back(GraphVariable{ "ticks", PinType::Number, 0.0 });
    auto increment = [&](const char* var, f64 by) {
        const u32 set = g.add_node("action.set_variable");
        g.find(set)->variable = var;
        const u32 get = g.add_node("data.get_variable");
        g.find(get)->variable = var;
        const u32 add = g.add_node("math.add");
        g.find(add)->values["b"] = by;
        g.connect(get, "value", add, "a");
        g.connect(add, "result", set, "value");
        return set;
    };
    // Space: Sequence -> count += 1, then Delay 0.5 -> count += 10 and spawn "Pop".
    const u32 key = g.add_node("event.key_pressed");
    const u32 seq = g.add_node("flow.sequence");
    g.connect(key, "then", seq, "in");
    g.connect(seq, "then 1", increment("count", 1.0), "in");
    const u32 delay = g.add_node("flow.delay");
    g.find(delay)->values["seconds"] = 0.5;
    g.connect(seq, "then 2", delay, "in");
    const u32 add10 = increment("count", 10.0);
    g.connect(delay, "then", add10, "in");
    const u32 spawn = g.add_node("action.spawn");
    g.find(spawn)->values["name"] = std::string("Pop");
    g.connect(add10, "then", spawn, "in");
    // Every 0.25 s: ticks += 1.
    const u32 timer = g.add_node("event.timer");
    g.find(timer)->values["seconds"] = 0.25;
    g.connect(timer, "then", increment("ticks", 1.0), "in");
    REQUIRE(compile_graph_to_lua(g).ok());
    f.write("keys.aegraph", graph_to_json(g));

    ScriptVM&    vm = f.start();
    const Entity e = f.spawn("keys.aegraph");
    InputState   in{};
    vm.set_input(&in);
    vm.update(0.1f);
    CHECK(std::get<f64>(*vm.get_field(e, "count")) == 0.0);
    in.keys[static_cast<usize>(Key::Space)] = ButtonState::Pressed;
    vm.update(0.1f);
    in.keys[static_cast<usize>(Key::Space)] = ButtonState::Held;
    CHECK(std::get<f64>(*vm.get_field(e, "count")) == 1.0);
    for (int i = 0; i < 6; ++i) {
        vm.update(0.1f);
    }
    CHECK(std::get<f64>(*vm.get_field(e, "count")) == 11.0);
    CHECK((scene::find_by_name(f.world, "Pop") != kNullEntity));
    CHECK(std::get<f64>(*vm.get_field(e, "ticks")) >= 2.0);
    CHECK(vm.error_count() == 0);

    // A broken edit keeps the previous version; a fixed one reloads (graphs are just scripts).
    f.write("keys.aegraph", "{ broken");
    CHECK(vm.reload_changed() == 0);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Running);
    VisualGraph g2 = g;
    g2.variables[0].value = 100.0;
    f.write("keys.aegraph", graph_to_json(g2));
    CHECK(vm.reload_changed() == 1);
    vm.set_input(nullptr);
}
