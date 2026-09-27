// test_anim_view.cpp — the animation panel's view model built from a live graph instance.
#include "aether/animation/anim_graph.h"
#include "aether/animation/clip.h"
#include "aether/animation/skeleton.h"
#include "aether/editor/anim_view.h"

#include <doctest/doctest.h>

#include <memory>

using namespace aether;
using namespace aether::animation;
using namespace aether::editor;

namespace {
std::shared_ptr<const Skeleton> one_joint_skeleton() {
    assets::SkeletonData d;
    d.joint_names.push_back("root");
    d.parents.push_back(-1);
    d.bind_local.push_back(Transform{});
    auto r = Skeleton::create(d);
    REQUIRE(r.has_value());
    return std::make_shared<const Skeleton>(std::move(r.value()));
}

std::shared_ptr<const AnimationClip> clip(f32 x) {
    assets::AnimationClipData d;
    d.name     = "c";
    d.duration = 1.0f;
    assets::AnimationChannel c;
    c.joint         = 0;
    c.path          = assets::AnimPath::Translation;
    c.interpolation = assets::Interpolation::Linear;
    c.times         = { 0.0f, 1.0f };
    c.values        = { Vec4(0.0f), Vec4(x, 0.0f, 0.0f, 0.0f) };
    d.channels.push_back(c);
    auto r = AnimationClip::create(d, 1);
    REQUIRE(r.has_value());
    return std::make_shared<const AnimationClip>(std::move(r.value()));
}
} // namespace

TEST_CASE("Anim view: invalid instance") {
    const AnimGraphInstance none;
    const AnimatorView      v = describe_animator(none);
    CHECK_FALSE(v.valid);
    CHECK(v.machines.empty());
}

TEST_CASE("Anim view: state machine, parameters and a live transition") {
    AnimGraphBuilder b(one_joint_skeleton());
    const ParamId    speed = b.add_float("speed");
    const ParamId    armed = b.add_bool("armed");
    b.add_trigger("jump");
    const NodeId  sm   = b.add_state_machine();
    const StateId idle = b.add_state(sm, "idle", b.add_clip(clip(0.0f)));
    const StateId walk = b.add_state(sm, "walk", b.add_clip(clip(1.0f)));
    TransitionOptions fade;
    fade.duration = 0.5f;
    b.add_transition(sm, idle, walk, fade).when_greater(speed, 0.5f);
    auto graph = b.build(sm);
    REQUIRE(graph.has_value());

    AnimGraphInstance inst(graph.value());
    inst.set_bool(armed, true);
    inst.update(0.1f);
    AnimatorView v = describe_animator(inst);
    REQUIRE(v.valid);
    CHECK(v.root == sm);
    REQUIRE(v.parameters.size() == 3);
    CHECK(v.parameters[0].name == "speed");
    CHECK(v.parameters[0].kind == AnimParamKind::Float);
    CHECK(v.parameters[1].kind == AnimParamKind::Bool);
    CHECK(v.parameters[1].value == 1.0f);
    CHECK(v.parameters[2].kind == AnimParamKind::Trigger);
    REQUIRE(v.machines.size() == 1);
    CHECK(v.machines[0].node == sm);
    REQUIRE(v.machines[0].states.size() == 2);
    CHECK(v.machines[0].current == idle);
    CHECK(v.machines[0].states[idle].current);
    CHECK_FALSE(v.machines[0].in_transition);

    inst.set_float(speed, 1.0f);
    inst.update(0.1f); // starts the 0.5 s crossfade
    inst.update(0.1f);
    v = describe_animator(inst);
    REQUIRE(v.machines.size() == 1);
    const AnimStateMachineView& m = v.machines[0];
    CHECK(m.current == walk);
    CHECK(m.in_transition);
    CHECK(m.source == idle);
    CHECK(m.states[idle].transition_source);
    CHECK(m.alpha > 0.0f);
    CHECK(m.alpha < 1.0f);
    CHECK(v.parameters[0].value == 1.0f);
}
