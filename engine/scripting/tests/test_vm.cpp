// test_vm.cpp — ScriptVM: lifecycle, properties, sandbox, isolation, budgets, hot reload,
// require, timers, events and console evaluation.
#include "script_test_utils.h"

#include "aether/scene/components.h"

#include <doctest/doctest.h>

#include <string>

using namespace aether;
using namespace aether::scripting;
using aether::scripting::test::ScriptFixture;

TEST_CASE("lifecycle callbacks run in order with dt") {
    ScriptFixture f;
    f.write("counter.lua", R"(
        function on_start(self) self.started = (self.started or 0) + 1 end
        function on_update(self, dt) self.updates = (self.updates or 0) + 1; self.last_dt = dt end
        function on_fixed_update(self, dt) self.fixed = (self.fixed or 0) + 1 end
        function on_destroy(self) destroyed_count = (destroyed_count or 0) + 1 end
    )");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("counter.lua");
    CHECK(vm.instance_state(e) == ScriptInstanceState::None); // attached at next update

    vm.update(0.5f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Running);
    CHECK(vm.instance_count() == 1);
    vm.fixed_update(1.0f / 60.0f);
    vm.update(0.25f);

    CHECK(std::get<i64>(*vm.get_field(e, "started")) == 1);
    CHECK(std::get<i64>(*vm.get_field(e, "updates")) == 2);
    CHECK(std::get<i64>(*vm.get_field(e, "fixed")) == 1);
    CHECK(std::get<f64>(*vm.get_field(e, "last_dt")) == doctest::Approx(0.25));
    CHECK(vm.frame_count() == 2);
    CHECK(vm.script_time() == doctest::Approx(0.75));

    f.world.remove<ScriptComponent>(e);
    vm.update(0.1f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::None);
    CHECK(vm.instance_count() == 0);
}

TEST_CASE("on_destroy runs when the entity is destroyed and when the VM dies") {
    ScriptFixture f;
    f.write("marker.lua", R"(
        function on_start(self) end
        function on_destroy(self) world.find("Marker"):set_tag(7) end
    )");
    ScriptVM&    vm     = f.start();
    const Entity marker = f.world.create("Marker");
    const Entity a      = f.spawn("marker.lua", "A");
    vm.update(0.016f);
    f.world.destroy(a);
    vm.update(0.016f);
    CHECK(f.world.get<TagComponent>(marker).tag == 7);
    CHECK(vm.error_count() == 0);

    f.world.get<TagComponent>(marker).tag = 0;
    f.spawn("marker.lua", "B");
    vm.update(0.016f);
    f.vm.reset(); // destroying the VM runs on_destroy for started instances
    CHECK(f.world.get<TagComponent>(marker).tag == 7);
}

TEST_CASE("properties: defaults, overrides and declared_properties") {
    ScriptFixture f;
    f.write("mover.lua", R"(
        properties = { speed = 2.5, label = "box", count = 3, dir = Vec3(0, 1, 0), enabled = true }
        function on_start(self) self.seen_speed = self.speed end
    )");
    ScriptVM& vm = f.start();

    auto declared = vm.declared_properties("mover");
    REQUIRE(declared);
    REQUIRE(declared->size() == 5);
    CHECK((*declared)[0].name == "count"); // sorted by name
    CHECK(std::get<i64>((*declared)[0].value) == 3);
    CHECK(std::get<Vec3>((*declared)[1].value) == Vec3(0, 1, 0));

    const Entity e = f.world.create("Mover");
    ScriptComponent c;
    c.script = "mover.lua";
    c.set_property("speed", 10.0);
    f.world.add<ScriptComponent>(e, c);
    vm.update(0.016f);
    CHECK(std::get<f64>(*vm.get_field(e, "seen_speed")) == doctest::Approx(10.0));
    CHECK(std::get<std::string>(*vm.get_field(e, "label")) == "box");

    // Table defaults are deep-copied per instance.
    f.write("list.lua", R"(
        properties = { items = { 1, 2 } }
        function on_start(self) table.insert(self.items, 3); self.n = #self.items end
    )");
    const Entity x = f.spawn("list.lua");
    const Entity y = f.spawn("list.lua");
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(x, "n")) == 3);
    CHECK(std::get<i64>(*vm.get_field(y, "n")) == 3);

    REQUIRE(vm.set_field(e, "speed", 1.5));
    CHECK(std::get<f64>(*vm.get_field(e, "speed")) == doctest::Approx(1.5));
    CHECK(std::holds_alternative<std::monostate>(*vm.get_field(e, "items"))); // absent -> nil
    CHECK_FALSE(vm.get_field(f.world.create("NoScript"), "speed").has_value()); // no instance
}

TEST_CASE("sandbox hides unsafe libraries") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    for (const char* name : { "io", "package", "debug", "dofile", "loadfile", "require_native" }) {
        CHECK(std::holds_alternative<std::monostate>(f.eval(std::string("_G.") + name)));
    }
    CHECK(std::holds_alternative<std::monostate>(f.eval("os.execute")));
    CHECK(std::holds_alternative<std::monostate>(f.eval("os.getenv")));
    CHECK(std::holds_alternative<std::monostate>(f.eval("string.dump")));
    CHECK(std::holds_alternative<f64>(f.eval("os.clock()")));

    // Shared tables are read-only.
    CHECK_FALSE(vm.run_string("math.pi = 3"));
    CHECK_FALSE(vm.run_string("rawset(math, 'x', 1)"));
    CHECK_FALSE(vm.run_string("Vec3.zero = nil"));
    CHECK(f.eval_as<f64>("math.pi") == doctest::Approx(3.14159265));

    // Binary chunks are refused; text chunks load into a fresh sandbox environment.
    CHECK(std::holds_alternative<std::monostate>(f.eval("load('return 1', 'x', 'b')")));
    CHECK(f.eval_as<i64>("load('return 1 + 1')()") == 2);

    // Metatables of engine types are protected.
    CHECK(std::get<bool>(f.eval("getmetatable(Vec3(1, 2, 3)) == false")));
    CHECK(std::get<bool>(f.eval("getmetatable('') == false")));

    // Globals do not leak between the console and script environments.
    f.write("leak.lua", "function on_start(self) self.saw = secret end\nleaked = true");
    f.run("secret = 42");
    const Entity e = f.spawn("leak.lua");
    vm.update(0.016f);
    CHECK(std::holds_alternative<std::monostate>(*vm.get_field(e, "saw")));
    CHECK(std::holds_alternative<std::monostate>(f.eval("leaked")));
}

TEST_CASE("a failing instance is disabled once; others keep running") {
    ScriptFixture f;
    f.write("bad.lua", "function on_update(self, dt) error('boom') end");
    f.write("good.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end");
    ScriptVM&    vm   = f.start();
    const Entity bad  = f.spawn("bad.lua");
    const Entity good = f.spawn("good.lua");
    for (int i = 0; i < 3; ++i) {
        vm.update(0.016f);
    }
    CHECK(vm.instance_state(bad) == ScriptInstanceState::Failed);
    CHECK(vm.instance_error(bad).find("boom") != std::string::npos);
    CHECK(vm.instance_error(bad).find("bad.lua:1") != std::string::npos);
    CHECK(vm.error_count() == 1);
    CHECK(std::get<i64>(*vm.get_field(good, "n")) == 3);
}

TEST_CASE("a script that fails to compile fails its instances with the load error") {
    ScriptFixture f;
    f.write("syntax.lua", "function on_update(self dt) end");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("syntax.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Failed);
    CHECK(vm.instance_error(e).find("syntax.lua") != std::string::npos);

    const Entity missing = f.spawn("does/not/exist.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(missing) == ScriptInstanceState::Failed);

    const Entity escape = f.spawn("../outside.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(escape) == ScriptInstanceState::None);
}

TEST_CASE("instruction budget kills runaway scripts, even inside pcall") {
    ScriptFixture f;
    f.write("spin.lua", "function on_update(self) while true do end end");
    f.write("sneaky.lua", R"(
        function on_update(self)
            for i = 1, 100 do pcall(function() while true do end end) end
            self.survived = true
        end
    )");
    ScriptVMConfig cfg;
    cfg.instruction_budget = 100'000;
    ScriptVM&    vm     = f.start(cfg);
    const Entity spin   = f.spawn("spin.lua");
    const Entity sneaky = f.spawn("sneaky.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(spin) == ScriptInstanceState::Failed);
    CHECK(vm.instance_error(spin).find("instruction budget") != std::string::npos);
    CHECK(vm.instance_state(sneaky) == ScriptInstanceState::Failed);
    CHECK_FALSE(vm.get_field(sneaky, "survived").value_or(ScriptValue{}).index() == 1);

    // The VM itself is still healthy.
    CHECK(f.eval_as<i64>("1 + 2") == 3);
}

TEST_CASE("memory limit turns runaway allocation into a script error") {
    ScriptFixture f;
    f.write("hog.lua", R"(
        function on_update(self)
            local t = {}
            for i = 1, 1e8 do t[i] = string.rep("x", 64) .. i end
        end
    )");
    ScriptVMConfig cfg;
    cfg.memory_limit       = usize{ 8 } << 20;
    cfg.instruction_budget = 0;
    ScriptVM&    vm        = f.start(cfg);
    const Entity e         = f.spawn("hog.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Failed);
    CHECK(vm.instance_error(e).find("memory") != std::string::npos);
    CHECK(f.eval_as<i64>("40 + 2") == 42);
}

TEST_CASE("hot reload keeps self, calls on_reload and survives a broken edit") {
    ScriptFixture f;
    f.write("hot.lua", R"(
        properties = { speed = 1 }
        function on_start(self) self.value = 1 end
        function on_update(self) self.version = 1 end
    )");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("hot.lua");
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(e, "version")) == 1);

    f.write("hot.lua", R"(
        properties = { speed = 1, extra = "new" }
        function on_start(self) self.value = 100 end
        function on_update(self) self.version = 2 end
        function on_reload(self) self.reloaded = true end
    )");
    CHECK(vm.reload_changed() == 1);
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(e, "version")) == 2);
    CHECK(std::get<i64>(*vm.get_field(e, "value")) == 1); // on_start did not run again
    CHECK(std::get<bool>(*vm.get_field(e, "reloaded")));
    CHECK(std::get<std::string>(*vm.get_field(e, "extra")) == "new");

    f.write("hot.lua", "function on_update(self) this is not lua");
    CHECK(vm.reload_changed() == 0);
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Running); // old version keeps running
    CHECK(std::get<i64>(*vm.get_field(e, "version")) == 2);
    CHECK_FALSE(vm.reload("hot"));
}

TEST_CASE("hot reload revives failed instances") {
    ScriptFixture f;
    f.write("fix.lua", "function on_update(self) error('not yet') end");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("fix.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Failed);
    f.write("fix.lua", "function on_start(self) self.ok = true end\nfunction on_update(self) end");
    REQUIRE(vm.reload("fix.lua"));
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Running);
    CHECK(std::get<bool>(*vm.get_field(e, "ok")));
}

TEST_CASE("require resolves modules under the root, caches them and reloads dependents") {
    ScriptFixture f;
    f.write("lib/easing.lua", "local M = {}\nfunction M.double(x) return x * 2 end\nreturn M");
    f.write("user.lua", R"(
        local easing = require("lib.easing")
        local again  = require("lib/easing.lua")
        same = easing == again
        function on_update(self) self.result = easing.double(21) end
    )");
    f.write("cycle_a.lua", "return require('cycle_b')");
    f.write("cycle_b.lua", "return require('cycle_a')");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("user.lua");
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(e, "result")) == 42);

    CHECK_FALSE(vm.run_string("require('cycle_a')"));
    CHECK_FALSE(vm.run_string("require('../escape')"));
    CHECK_FALSE(vm.run_string("require('missing.module')"));

    f.write("lib/easing.lua", "local M = {}\nfunction M.double(x) return x * 20 end\nreturn M");
    CHECK(vm.reload_changed() >= 1);
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(e, "result")) == 420);
}

TEST_CASE("timers fire on script time and can be cancelled") {
    ScriptFixture f;
    f.write("timers.lua", R"(
        function on_start(self)
            self.once, self.ticks = 0, 0
            timer.after(1.0, function() self.once = self.once + 1 end)
            self.rep = timer.every(0.5, function(h) self.ticks = self.ticks + 1 end)
            local c = timer.after(0.1, function() self.cancelled_fired = true end)
            c:cancel()
        end
    )");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("timers.lua");
    vm.update(0.25f); // t = 0.25: on_start schedules (every: 0.75, after: 1.25)
    CHECK(std::get<i64>(*vm.get_field(e, "ticks")) == 0);
    vm.update(0.3f); // t = 0.55
    CHECK(std::get<i64>(*vm.get_field(e, "ticks")) == 0);
    vm.update(0.5f); // t = 1.05
    CHECK(std::get<i64>(*vm.get_field(e, "ticks")) == 1);
    CHECK(std::get<i64>(*vm.get_field(e, "once")) == 0);
    vm.update(0.25f); // t = 1.30
    CHECK(std::get<i64>(*vm.get_field(e, "once")) == 1);
    CHECK(std::get<i64>(*vm.get_field(e, "ticks")) == 2);
    CHECK(std::holds_alternative<std::monostate>(*vm.get_field(e, "cancelled_fired")));

    f.run("x = nil"); // console works while timers are pending
    vm.update(5.0f);  // a long frame fires a repeating timer only once
    CHECK(std::get<i64>(*vm.get_field(e, "ticks")) == 3);
    CHECK(std::get<i64>(*vm.get_field(e, "once")) == 1);

    // Timers die with their instance.
    f.world.remove<ScriptComponent>(e);
    vm.update(0.016f);
    vm.update(1.0f);
    CHECK(vm.error_count() == 0);
}

TEST_CASE("timer callback errors disable the owning instance") {
    ScriptFixture f;
    f.write("bad_timer.lua", "function on_start(self) timer.after(0, function() error('late') end) end");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("bad_timer.lua");
    vm.update(0.016f);
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Failed);
    CHECK(vm.instance_error(e).find("late") != std::string::npos);
}

TEST_CASE("events: script subscribers, C++ emit with payload, failing handlers removed") {
    ScriptFixture f;
    f.write("listener.lua", R"(
        function on_start(self)
            self.hits = 0
            self.sub = events.subscribe("hit", function(payload, name)
                self.hits = self.hits + payload.damage
                self.last_name = name
                self.from = payload.source
            end)
        end
    )");
    f.write("broken.lua", "function on_start(self) events.subscribe('hit', function() error('bad handler') end) end");
    ScriptVM&    vm       = f.start();
    const Entity listener = f.spawn("listener.lua");
    const Entity broken   = f.spawn("broken.lua");
    const Entity source   = f.world.create("Source");
    vm.update(0.016f);

    const ScriptProperty payload[] = { { "damage", i64{ 5 } }, { "source", source } };
    CHECK(vm.emit_event("hit", payload) == 1);
    CHECK(std::get<i64>(*vm.get_field(listener, "hits")) == 5);
    CHECK(std::get<std::string>(*vm.get_field(listener, "last_name")) == "hit");
    CHECK((std::get<Entity>(*vm.get_field(listener, "from")) == source));
    CHECK(vm.instance_state(broken) == ScriptInstanceState::Failed);

    CHECK(vm.emit_event("hit", payload) == 1);
    CHECK(f.eval_as<i64>("events.count('hit')") == 1);
    CHECK(vm.emit_event("nobody_listens") == 0);

    // Scripts can emit to each other, and subscriptions die with the instance.
    f.run("events.emit('hit', { damage = 10 })");
    CHECK(std::get<i64>(*vm.get_field(listener, "hits")) == 20);
    f.world.destroy(listener);
    vm.update(0.016f);
    CHECK(f.eval_as<i64>("events.count('hit')") == 0);
}

TEST_CASE("console: run_string, evaluate, get_global") {
    ScriptFixture f;
    ScriptVM&     vm = f.start();
    f.run("answer = 6 * 7");
    CHECK(std::get<i64>(*vm.get_global("answer")) == 42);
    CHECK(f.eval_as<std::string>("'a' .. 'b'") == "ab");
    CHECK(f.eval_as<bool>("answer == 42"));

    auto syntax = vm.run_string("this is not lua");
    CHECK_FALSE(syntax);
    CHECK(syntax.error().code == ErrorCode::CompilationFailed);
    auto runtime = vm.run_string("error('oops')");
    CHECK_FALSE(runtime);
    CHECK(runtime.error().message.find("oops") != std::string::npos);
    auto table = vm.evaluate("{}");
    CHECK_FALSE(table);
    CHECK(table.error().code == ErrorCode::Unsupported);
    CHECK(vm.memory_used() > 0);
}

TEST_CASE("disabled components keep their instance but get no callbacks") {
    ScriptFixture f;
    f.write("tick.lua", "function on_update(self) self.n = (self.n or 0) + 1 end");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("tick.lua");
    vm.update(0.016f);
    f.world.get<ScriptComponent>(e).enabled = false;
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::Inactive);
    CHECK(std::get<i64>(*vm.get_field(e, "n")) == 1);
    f.world.get<ScriptComponent>(e).enabled = true;
    vm.update(0.016f);
    CHECK(std::get<i64>(*vm.get_field(e, "n")) == 2);
}

TEST_CASE("changing the script path swaps the instance") {
    ScriptFixture f;
    f.write("a.lua", "function on_start(self) self.kind = 'a' end");
    f.write("b.lua", "function on_start(self) self.kind = 'b' end");
    ScriptVM&    vm = f.start();
    const Entity e  = f.spawn("a.lua");
    vm.update(0.016f);
    CHECK(std::get<std::string>(*vm.get_field(e, "kind")) == "a");
    f.world.get<ScriptComponent>(e).script = "b.lua";
    vm.update(0.016f); // detected (and re-attached) during this update
    vm.update(0.016f);
    CHECK(std::get<std::string>(*vm.get_field(e, "kind")) == "b");
}

TEST_CASE("manual attach mode") {
    ScriptFixture f;
    f.write("m.lua", "function on_update(self) self.ran = true end");
    ScriptVMConfig cfg;
    cfg.auto_attach = false;
    ScriptVM&    vm = f.start(cfg);
    const Entity e  = f.spawn("m.lua");
    vm.update(0.016f);
    CHECK(vm.instance_state(e) == ScriptInstanceState::None);
    REQUIRE(vm.attach(e));
    vm.update(0.016f);
    CHECK(std::get<bool>(*vm.get_field(e, "ran")));
    vm.detach(e);
    CHECK(vm.instance_count() == 0);
    CHECK_FALSE(vm.attach(f.world.create("NoScript")));
}
