// test_input_map.cpp — InputMap actions/axes, edges, JSON, key names and the InputSubsystem.
#include "aether/gameplay/input_map.h"
#include "aether/gameplay/input_subsystem.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::gameplay;

namespace {
InputState with_keys(std::initializer_list<Key> keys, ButtonState state = ButtonState::Held) {
    InputState s{};
    for (const Key k : keys) {
        s.keys[static_cast<usize>(k)] = state;
    }
    return s;
}
} // namespace

TEST_CASE("actions: any binding holds the action; edges fire once") {
    InputMap map;
    map.bind_action("Jump", Key::Space);
    map.bind_action("Jump", MouseButton::Right);
    map.bind_action("Jump", Key::Space); // duplicate ignored
    REQUIRE(map.action_bindings("Jump") != nullptr);
    CHECK(map.action_bindings("Jump")->size() == 2);

    map.update(with_keys({ Key::Space }, ButtonState::Pressed));
    CHECK(map.action_down("Jump"));
    CHECK(map.action_pressed("Jump"));
    // Second binding goes down too: still one continuous press, no new edge.
    InputState both = with_keys({ Key::Space });
    both.mouse[static_cast<usize>(MouseButton::Right)] = ButtonState::Pressed;
    map.update(both);
    CHECK(map.action_down("Jump"));
    CHECK_FALSE(map.action_pressed("Jump"));
    map.update(InputState{});
    CHECK(map.action_released("Jump"));
    CHECK_FALSE(map.action_down("Jump"));
    map.update(InputState{});
    CHECK_FALSE(map.action_released("Jump"));
    CHECK_FALSE(map.action_down("Unknown"));
}

TEST_CASE("axes: sum of keys, mouse delta and scroll") {
    InputMap map;
    map.bind_axis("MoveForward", Key::W, 1.0f);
    map.bind_axis("MoveForward", Key::S, -1.0f);
    map.bind_axis("LookX", InputSourceKind::MouseDeltaX, 0.5f);
    map.bind_axis("Zoom", InputSourceKind::ScrollY);

    InputState s = with_keys({ Key::W });
    s.cursor_delta = Vec2(10.0f, 3.0f);
    s.scroll       = Vec2(0.0f, -2.0f);
    map.update(s);
    CHECK(map.axis("MoveForward") == doctest::Approx(1.0f));
    CHECK(map.axis("LookX") == doctest::Approx(5.0f));
    CHECK(map.axis("Zoom") == doctest::Approx(-2.0f));
    map.update(with_keys({ Key::W, Key::S }));
    CHECK(map.axis("MoveForward") == doctest::Approx(0.0f));
    CHECK(map.axis("Nothing") == 0.0f);

    map.reset_state();
    CHECK(map.axis("LookX") == 0.0f);
    CHECK(map.unbind("Zoom"));
    CHECK_FALSE(map.unbind("Zoom"));
    CHECK(map.axis_names() == std::vector<std::string>{ "LookX", "MoveForward" });
}

TEST_CASE("reset_state releases actions with an edge on the next update") {
    InputMap map;
    map.bind_action("Fire", Key::F);
    map.update(with_keys({ Key::F }));
    map.reset_state();
    CHECK_FALSE(map.action_down("Fire"));
    map.update(InputState{});
    CHECK_FALSE(map.action_pressed("Fire"));
}

TEST_CASE("key names round-trip for every named key") {
    int named = 0;
    for (u16 k = 0; k < static_cast<u16>(Key::Count); ++k) {
        const auto       key  = static_cast<Key>(k);
        const StringView name = key_name(key);
        if (name == "Unknown") {
            continue;
        }
        ++named;
        const auto back = key_from_name(name);
        REQUIRE(back.has_value());
        CHECK(*back == key);
    }
    CHECK(named >= 85); // letters, digits, F1-F12 and the named keys
    CHECK(key_from_name("space") == Key::Space); // case-insensitive
    CHECK(key_from_name("7") == Key::Num7);
    CHECK_FALSE(key_from_name("Hyperdrive").has_value());
    CHECK(binding_input_name(InputBinding::from_mouse_button(MouseButton::Middle)) == "Mouse:Middle");
    CHECK(binding_from_input_name("mouse:button4")->button == MouseButton::Button4);
    CHECK(binding_from_input_name("scrolly", 2.0f)->scale == 2.0f);
    CHECK_FALSE(binding_from_input_name("Mouse:Tail").has_value());
}

TEST_CASE("JSON load/save round trip; unknown inputs skipped; malformed documents rejected") {
    InputMap map;
    auto     loaded = map.load_json(R"({
        "actions": { "Jump": ["Space"], "Fire": ["Mouse:Left", "LeftControl", "NotAKey"] },
        "axes": {
            "MoveForward": [ { "input": "W", "scale": 1 }, { "input": "S", "scale": -1 } ],
            "Zoom": [ { "input": "ScrollY" } ],
            "Turn": [ "MouseDeltaX" ]
        } })");
    REQUIRE(loaded.has_value());
    CHECK(*loaded == 1);
    CHECK(map.has_action("Fire"));
    CHECK(map.action_bindings("Fire")->size() == 2);
    CHECK(map.axis_bindings("MoveForward")->at(1).scale == -1.0f);
    CHECK(map.axis_bindings("Zoom")->at(0).scale == 1.0f);

    InputMap copy;
    REQUIRE(copy.load_json(map.to_json()).has_value());
    CHECK(copy.action_names() == map.action_names());
    CHECK(copy.axis_names() == map.axis_names());
    CHECK(*copy.axis_bindings("MoveForward") == *map.axis_bindings("MoveForward"));

    CHECK_FALSE(copy.load_json("not json"));
    CHECK_FALSE(copy.load_json(R"({ "actions": [] })"));
    CHECK_FALSE(copy.load_json(R"({ "axes": { "X": "W" } })"));
    CHECK(copy.has_action("Jump")); // unchanged after failures
    CHECK_FALSE(copy.load_file("/definitely/not/here.json"));
}

TEST_CASE("default camera bindings") {
    InputMap map;
    map.add_default_camera_bindings();
    for (const char* axis : { "Camera.MoveForward", "Camera.MoveRight", "Camera.MoveUp", "Camera.LookX",
                              "Camera.LookY", "Camera.Zoom" }) {
        CHECK(map.has_axis(axis));
    }
    for (const char* action : { "Camera.Look", "Camera.Pan", "Camera.Boost" }) {
        CHECK(map.has_action(action));
    }
}

TEST_CASE("InputSubsystem masks what ImGui captures") {
    InputSubsystem sub;
    sub.input_map().bind_action("Jump", Key::Space);
    sub.input_map().bind_action("Shoot", MouseButton::Left);
    InputState raw = with_keys({ Key::Space });
    raw.mouse[static_cast<usize>(MouseButton::Left)] = ButtonState::Held;
    raw.cursor_delta = Vec2(4.0f, 0.0f);

    sub.feed(raw, true, false);
    CHECK_FALSE(sub.input_map().action_down("Jump"));
    CHECK(sub.input_map().action_down("Shoot"));
    sub.feed(raw, false, true);
    CHECK(sub.input_map().action_down("Jump"));
    CHECK_FALSE(sub.input_map().action_down("Shoot"));
    CHECK(sub.state().cursor_delta == Vec2(0.0f));
    CHECK(sub.input_map().axis("Camera.LookX") == 0.0f);

    sub.set_respect_imgui_capture(false);
    sub.feed(raw, true, true);
    CHECK(sub.input_map().action_down("Jump"));
    CHECK(sub.input_map().axis("Camera.LookX") == doctest::Approx(4.0f));

    // Without a window the subsystem sees no input at all.
    EngineContext engine;
    FrameContext  frame;
    frame.engine = &engine;
    sub.on_begin_frame(frame);
    CHECK_FALSE(sub.input_map().action_down("Jump"));
}
