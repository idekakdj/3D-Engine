// test_graph.cpp — AnimGraph building, blend trees, state machines, notifies, sync, additive.
#include "aether/animation/anim_graph.h"
#include "anim_test_utils.h"

#include <vector>

using namespace anim_test;

namespace {

// Evaluates the instance and returns joint 0's translation.
Vec3 root_position(AnimGraphInstance& inst, u32 joints) {
    std::vector<Transform> pose(joints);
    inst.evaluate(pose);
    return pose[0].position;
}

std::shared_ptr<const AnimationClip> clip_with_notify(u32 joints, f32 duration, const char* notify,
                                                      f32 at) {
    assets::AnimationClipData d;
    d.name = "notify_clip";
    d.duration = duration;
    d.channels.push_back(channel(0, AnimPath::Translation, Interpolation::Linear, {0.0f}, {Vec4(0)}));
    AnimationClip clip = make_clip_value(d, joints);
    clip.add_notify(notify, at);
    return std::make_shared<const AnimationClip>(std::move(clip));
}

// Joint 0 moves along +x at 1 m/s over `duration` seconds.
std::shared_ptr<const AnimationClip> moving_clip(u32 joints, f32 duration) {
    assets::AnimationClipData d;
    d.name = "move";
    d.channels.push_back(channel(0, AnimPath::Translation, Interpolation::Linear, {0.0f, duration},
                                 {Vec4(0), Vec4(duration, 0, 0, 0)}));
    return std::make_shared<const AnimationClip>(make_clip_value(d, joints));
}

} // namespace

TEST_CASE("Graph: single clip") {
    const auto sk   = make_skeleton(chain_data(2));
    auto       made = AnimGraph::make_single_clip(sk, moving_clip(2, 2.0f), WrapMode::Loop, 1.0f);
    REQUIRE(made.has_value());
    const auto& graph = made.value();
    CHECK(graph->node_count() == 1);
    CHECK(&graph->skeleton() == sk.get());

    AnimGraphInstance inst(graph);
    REQUIRE(inst.valid());
    CHECK(near(root_position(inst, 2), Vec3(0.0f))); // evaluate before update = update(0)
    inst.update(0.5f);
    CHECK(near(root_position(inst, 2), Vec3(0.5f, 0, 0)));
    CHECK(inst.clip_time(graph->root()) == doctest::Approx(0.5f));
    inst.update(2.0f); // loops: 2.5 -> 0.5
    CHECK(near(root_position(inst, 2), Vec3(0.5f, 0, 0)));
    inst.reset();
    CHECK(near(root_position(inst, 2), Vec3(0.0f)));

    // Joint 1 keeps its bind pose.
    std::vector<Transform> pose(2);
    inst.evaluate(pose);
    CHECK(near(pose[1].position, Vec3(0, 1, 0)));
}

TEST_CASE("Graph: parameters") {
    const auto       sk = make_skeleton(chain_data(1));
    AnimGraphBuilder b(sk);
    const ParamId    speed = b.add_float("speed", 1.5f);
    const ParamId    grounded = b.add_bool("grounded", true);
    const ParamId    jump = b.add_trigger("jump");
    CHECK(b.add_float("speed") == kInvalidParam); // duplicate name
    const NodeId clip = b.add_clip(x_clip(1, 1.0f));
    // The first error is reported by build().
    CHECK_FALSE(b.build(clip).has_value());

    AnimGraphBuilder ok(sk);
    const ParamId    s2 = ok.add_float("speed", 1.5f);
    ok.add_bool("grounded", true);
    ok.add_trigger("jump");
    auto graph = ok.build(ok.add_clip(x_clip(1, 1.0f)));
    REQUIRE(graph.has_value());
    CHECK(graph.value()->parameter_count() == 3);
    CHECK(graph.value()->find_parameter("grounded") == 1);
    CHECK(graph.value()->find_parameter("nope") == kInvalidParam);
    CHECK(graph.value()->parameter_type(2) == ParamType::Trigger);
    CHECK(graph.value()->parameter_name(s2) == "speed");

    AnimGraphInstance inst(graph.value());
    CHECK(inst.get_float(s2) == doctest::Approx(1.5f));
    CHECK(inst.get_bool(1));
    CHECK(inst.set_float("speed", 3.0f));
    CHECK_FALSE(inst.set_float("grounded", 1.0f)); // wrong type
    CHECK_FALSE(inst.set_float("missing", 1.0f));
    CHECK(inst.set_trigger("jump"));
    CHECK(inst.is_trigger_set(2));
    inst.reset_trigger(2);
    CHECK_FALSE(inst.is_trigger_set(2));
    inst.reset_parameters();
    CHECK(inst.get_float(s2) == doctest::Approx(1.5f));
    (void)speed;
    (void)grounded;
    (void)jump;
}

TEST_CASE("Graph: Blend1D weights and pose") {
    const auto       sk = make_skeleton(chain_data(1));
    AnimGraphBuilder b(sk);
    const ParamId    speed = b.add_float("speed");
    const NodeId     idle = b.add_clip(x_clip(1, 0.0f));
    const NodeId     walk = b.add_clip(x_clip(1, 10.0f));
    const NodeId     run = b.add_clip(x_clip(1, 20.0f));
    const NodeId     blend = b.add_blend1d(speed, {{idle, 0.0f}, {walk, 1.0f}, {run, 3.0f}});
    auto             graph = b.build(blend);
    REQUIRE(graph.has_value());
    AnimGraphInstance inst(graph.value());

    inst.set_float(speed, 0.25f);
    inst.update(0.016f);
    CHECK(near(root_position(inst, 1), Vec3(2.5f, 0, 0)));
    CHECK(inst.node_weight(idle) == doctest::Approx(0.75f));
    CHECK(inst.node_weight(walk) == doctest::Approx(0.25f));
    CHECK(inst.node_weight(run) == doctest::Approx(0.0f));

    inst.set_float(speed, 2.0f);
    inst.update(0.016f);
    CHECK(near(root_position(inst, 1), Vec3(15.0f, 0, 0)));

    inst.set_float(speed, -5.0f); // clamped below the first threshold
    inst.update(0.016f);
    CHECK(near(root_position(inst, 1), Vec3(0.0f)));
    inst.set_float(speed, 50.0f); // and above the last
    inst.update(0.016f);
    CHECK(near(root_position(inst, 1), Vec3(20.0f, 0, 0)));
}

TEST_CASE("Graph: Blend2D reproduces sample points and blends inside the hull") {
    const auto       sk = make_skeleton(chain_data(1));
    AnimGraphBuilder b(sk);
    const ParamId    px = b.add_float("x");
    const ParamId    py = b.add_float("y");
    const NodeId     c = b.add_clip(x_clip(1, 0.0f));
    const NodeId     r = b.add_clip(pose_clip(1, 1.0f, {{0u, Vec3(10, 0, 0)}}));
    const NodeId     f = b.add_clip(pose_clip(1, 1.0f, {{0u, Vec3(0, 0, 10)}}));
    const NodeId     blend = b.add_blend2d(px, py, {{c, Vec2(0, 0)}, {r, Vec2(1, 0)}, {f, Vec2(0, 1)}});
    auto             graph = b.build(blend);
    REQUIRE(graph.has_value());
    AnimGraphInstance inst(graph.value());

    inst.set_float(px, 1.0f);
    inst.set_float(py, 0.0f);
    inst.update(0.0f);
    CHECK(near(root_position(inst, 1), Vec3(10, 0, 0)));

    inst.set_float(px, 0.25f);
    inst.set_float(py, 0.25f);
    inst.update(0.0f);
    CHECK(near(root_position(inst, 1), Vec3(2.5f, 0, 2.5f)));
    CHECK(inst.node_weight(c) + inst.node_weight(r) + inst.node_weight(f) == doctest::Approx(1.0f));
}

TEST_CASE("Graph: state machine transitions, crossfade and triggers") {
    const auto       sk = make_skeleton(chain_data(1));
    AnimGraphBuilder b(sk);
    const ParamId    speed = b.add_float("speed");
    const ParamId    jump = b.add_trigger("jump");
    const NodeId     sm = b.add_state_machine();
    const StateId    idle = b.add_state(sm, "idle", b.add_clip(x_clip(1, 0.0f)));
    const StateId    walk = b.add_state(sm, "walk", b.add_clip(x_clip(1, 10.0f)));
    const StateId    air = b.add_state(sm, "air", b.add_clip(x_clip(1, -5.0f)));
    TransitionOptions fade;
    fade.duration = 0.2f;
    b.add_transition(sm, idle, walk, fade).when_greater(speed, 0.5f);
    b.add_transition(sm, walk, idle, fade).when_less(speed, 0.5f);
    TransitionOptions instant;
    instant.duration = 0.0f;
    b.add_any_state_transition(sm, air, instant).when_triggered(jump);
    b.add_transition(sm, air, idle, instant).when_less(speed, 0.5f);
    auto graph = b.build(sm);
    REQUIRE(graph.has_value());
    CHECK(graph.value()->find_state(sm, "walk") == walk);
    CHECK(graph.value()->find_state(sm, "nope") == kInvalidState);
    CHECK(graph.value()->state_count(sm) == 3);

    AnimGraphInstance inst(graph.value());
    inst.update(0.1f);
    CHECK(inst.current_state(sm) == idle);
    CHECK_FALSE(inst.in_transition(sm));
    CHECK(near(root_position(inst, 1), Vec3(0.0f)));

    inst.set_float(speed, 1.0f);
    inst.update(0.1f);
    CHECK(inst.current_state(sm) == walk);
    CHECK(inst.in_transition(sm));
    const f32 x_mid = root_position(inst, 1).x;
    CHECK(x_mid > 0.0f);
    CHECK(x_mid < 10.0f);
    CHECK(inst.transition_alpha(sm) > 0.0f);
    CHECK(inst.transition_alpha(sm) < 1.0f);
    for (int i = 0; i < 5; ++i) {
        inst.update(0.1f);
    }
    CHECK_FALSE(inst.in_transition(sm));
    CHECK(inst.transition_alpha(sm) == doctest::Approx(1.0f));
    CHECK(near(root_position(inst, 1), Vec3(10, 0, 0)));

    // Any-state trigger: instant, and the trigger is consumed.
    inst.set_trigger(jump);
    inst.update(0.1f);
    CHECK(inst.current_state(sm) == air);
    CHECK_FALSE(inst.is_trigger_set(jump));
    CHECK(near(root_position(inst, 1), Vec3(-5, 0, 0)));
    inst.update(0.1f);
    CHECK(inst.current_state(sm) == air); // not re-entered: trigger consumed

    inst.set_float(speed, 0.0f);
    inst.update(0.1f);
    CHECK(inst.current_state(sm) == idle);
}

TEST_CASE("Graph: exit time") {
    const auto        sk = make_skeleton(chain_data(1));
    AnimGraphBuilder  b(sk);
    const NodeId      sm = b.add_state_machine();
    ClipNodeOptions   once;
    once.wrap = WrapMode::Clamp;
    const StateId     attack = b.add_state(sm, "attack", b.add_clip(x_clip(1, 1.0f, 1.0f), once));
    const StateId     recover = b.add_state(sm, "recover", b.add_clip(x_clip(1, 2.0f)));
    TransitionOptions opt;
    opt.duration = 0.0f;
    opt.has_exit_time = true;
    opt.exit_time = 1.0f;
    b.add_transition(sm, attack, recover, opt);
    auto graph = b.build(sm);
    REQUIRE(graph.has_value());

    AnimGraphInstance inst(graph.value());
    inst.update(0.4f);
    CHECK(inst.current_state(sm) == attack);
    // Progress is accumulated at the start of the NEXT update (one-frame latency).
    CHECK(inst.state_normalized_time(sm, attack) == doctest::Approx(0.0));
    inst.update(0.4f);
    CHECK(inst.current_state(sm) == attack);
    CHECK(inst.state_normalized_time(sm, attack) == doctest::Approx(0.4));
    int updates = 0;
    while (inst.current_state(sm) == attack && updates < 10) {
        inst.update(0.4f);
        ++updates;
    }
    CHECK(inst.current_state(sm) == recover);
    CHECK(updates <= 2); // reached 1.0 during the 3rd update; fires by the next
}

TEST_CASE("Graph: builder validation") {
    const auto sk = make_skeleton(chain_data(1));
    {
        AnimGraphBuilder b(sk);
        CHECK_FALSE(b.build(kInvalidNode).has_value());
    }
    {
        AnimGraphBuilder b(sk);
        const NodeId     sm = b.add_state_machine();
        CHECK_FALSE(b.build(sm).has_value()); // no states
    }
    {
        AnimGraphBuilder b(sk);
        const NodeId     clip = b.add_clip(x_clip(1, 1.0f));
        const ParamId    p = b.add_float("p");
        // The same node used twice: not a tree.
        const NodeId blend = b.add_blend1d(p, {{clip, 0.0f}, {clip, 1.0f}});
        CHECK_FALSE(b.build(blend).has_value());
    }
    {
        AnimGraphBuilder b(sk);
        const ParamId    flag = b.add_bool("flag");
        // A bool parameter cannot drive a 1D blend.
        const NodeId blend =
            b.add_blend1d(flag, {{b.add_clip(x_clip(1, 0.0f)), 0.0f}, {b.add_clip(x_clip(1, 1.0f)), 1.0f}});
        CHECK_FALSE(b.build(blend).has_value());
    }
    {
        // A clip animating more joints than the skeleton has.
        AnimGraphBuilder b(sk);
        CHECK_FALSE(b.build(b.add_clip(pose_clip(3, 1.0f, {{2u, Vec3(1.0f)}}))).has_value());
    }
}

TEST_CASE("Graph: notifies from the active clip") {
    const auto sk = make_skeleton(chain_data(1));
    auto       made = AnimGraph::make_single_clip(sk, clip_with_notify(1, 1.0f, "footstep", 0.5f));
    REQUIRE(made.has_value());
    AnimGraphInstance inst(made.value());
    inst.update(0.3f);
    CHECK(inst.notifies().empty());
    inst.update(0.3f);
    REQUIRE(inst.notifies().size() == 1);
    CHECK(inst.notifies()[0].name == "footstep");
    CHECK(inst.notifies()[0].time == doctest::Approx(0.5f));
    CHECK(inst.notifies()[0].weight == doctest::Approx(1.0f));
    inst.update(0.3f);
    CHECK(inst.notifies().empty());
    inst.update(1.0f); // one full loop later: fires again
    CHECK(inst.notifies().size() == 1);
}

TEST_CASE("Graph: sync groups keep cycles phase-aligned") {
    const auto        sk = make_skeleton(chain_data(1));
    AnimGraphBuilder  b(sk);
    const ParamId     speed = b.add_float("speed", 0.5f);
    const SyncGroupId feet = b.add_sync_group("feet");
    ClipNodeOptions   opt;
    opt.sync_group = feet;
    const NodeId walk = b.add_clip(moving_clip(1, 1.0f), opt); // 1 s cycle
    const NodeId run = b.add_clip(moving_clip(1, 0.5f), opt);  // 0.5 s cycle
    auto graph = b.build(b.add_blend1d(speed, {{walk, 0.0f}, {run, 1.0f}}));
    REQUIRE(graph.has_value());
    CHECK(graph.value()->find_sync_group("feet") == feet);

    AnimGraphInstance inst(graph.value());
    for (int i = 0; i < 7; ++i) {
        inst.update(0.1f);
        const f32 walk_phase = inst.clip_time(walk) / 1.0f;
        const f32 run_phase = inst.clip_time(run) / 0.5f;
        CHECK(walk_phase == doctest::Approx(run_phase).epsilon(1e-3));
        CHECK(static_cast<f32>(inst.sync_phase(feet)) - std::floor(static_cast<f32>(inst.sync_phase(feet))) ==
              doctest::Approx(walk_phase).epsilon(1e-3));
    }
}

TEST_CASE("Graph: additive and masked layers") {
    auto d = chain_data(2);
    const auto sk = make_skeleton(d);

    SUBCASE("additive over bind reference") {
        AnimGraphBuilder b(sk);
        const ParamId    w = b.add_float("w", 0.5f);
        const NodeId     base = b.add_clip(x_clip(2, 2.0f));
        const NodeId     add = b.add_clip(pose_clip(2, 1.0f, {{0u, Vec3(0, 4, 0)}}));
        AdditiveOptions  opt;
        opt.weight_param = w;
        auto graph = b.build(b.add_additive(base, add, opt));
        REQUIRE(graph.has_value());
        AnimGraphInstance inst(graph.value());
        inst.update(0.0f);
        CHECK(near(root_position(inst, 2), Vec3(2, 2, 0)));
        inst.set_float(w, 1.0f);
        inst.update(0.0f);
        CHECK(near(root_position(inst, 2), Vec3(2, 4, 0)));
    }
    SUBCASE("masked layer overrides only the masked branch") {
        AnimGraphBuilder b(sk);
        const NodeId     base = b.add_clip(pose_clip(2, 1.0f, {{0u, Vec3(1, 0, 0)}, {1u, Vec3(0, 1, 0)}}));
        const NodeId     layer = b.add_clip(pose_clip(2, 1.0f, {{0u, Vec3(9, 9, 9)}, {1u, Vec3(0, 5, 0)}}));
        auto graph = b.build(b.add_masked_layer(base, layer, BoneMask::from_branch(*sk, 1u)));
        REQUIRE(graph.has_value());
        AnimGraphInstance      inst(graph.value());
        std::vector<Transform> pose(2);
        inst.update(0.0f);
        inst.evaluate(pose);
        CHECK(near(pose[0].position, Vec3(1, 0, 0)));
        CHECK(near(pose[1].position, Vec3(0, 5, 0)));
    }
}

TEST_CASE("Graph: instance copies duplicate playback state") {
    const auto sk = make_skeleton(chain_data(1));
    auto       made = AnimGraph::make_single_clip(sk, moving_clip(1, 4.0f));
    REQUIRE(made.has_value());
    AnimGraphInstance a(made.value());
    a.update(1.0f);
    AnimGraphInstance b = a;
    b.update(1.0f);
    CHECK(near(root_position(a, 1), Vec3(1, 0, 0)));
    CHECK(near(root_position(b, 1), Vec3(2, 0, 0)));
    AnimGraphInstance moved = std::move(b);
    CHECK(moved.valid());
    CHECK_FALSE(AnimGraphInstance{}.valid());
}
