// test_animator.cpp — AnimatorComponent pipeline, AnimationSubsystem and the Animator codec.
#include "aether/animation/animation_subsystem.h"
#include "aether/animation/components.h"
#include "aether/animation/pose.h"
#include "aether/animation/serialization.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "anim_test_utils.h"

#include <vector>

using namespace anim_test;

namespace {

std::shared_ptr<const AnimationClip> walk_clip(u32 joints) {
    assets::AnimationClipData d;
    d.name = "walk";
    d.channels.push_back(channel(0, AnimPath::Translation, Interpolation::Linear, {0.0f, 1.0f},
                                 {Vec4(0), Vec4(0, 0, 2, 0)}));
    return std::make_shared<const AnimationClip>(make_clip_value(d, joints));
}

std::shared_ptr<const AnimGraph> single(const std::shared_ptr<const Skeleton>& sk,
                                        std::shared_ptr<const AnimationClip> clip) {
    auto g = AnimGraph::make_single_clip(sk, std::move(clip));
    REQUIRE(g.has_value());
    return g.value();
}

} // namespace

TEST_CASE("Animator: bind outputs, tick, pause and rate") {
    const auto sk = make_skeleton(chain_data(3));
    AnimatorComponent anim(sk);
    REQUIRE(anim.palette().size() == 3);
    for (const Mat4& m : anim.palette()) {
        CHECK(near(m, Mat4(1.0f))); // bind pose -> identity palette
    }
    anim.tick(0.1f); // no graph: stays in the bind pose
    CHECK(near(Vec3(anim.model_pose()[2][3]), Vec3(0, 2, 0)));

    REQUIRE(anim.set_graph(single(sk, walk_clip(3))));
    anim.tick(0.25f);
    CHECK(near(anim.local_pose()[0].position, Vec3(0, 0, 0.5f)));
    CHECK(near(Vec3(anim.model_pose()[2][3]), Vec3(0, 2, 0.5f)));
    CHECK(near(anim.palette()[0], anim.model_pose()[0] * sk->inverse_bind()[0]));

    anim.set_paused(true);
    anim.tick(0.25f);
    CHECK(near(anim.local_pose()[0].position, Vec3(0, 0, 0.5f)));
    anim.set_paused(false);
    anim.set_playback_rate(2.0f);
    anim.tick(0.125f); // 0.25 s of clip time
    CHECK(near(anim.local_pose()[0].position, Vec3(0, 0, 1.0f)));

    // A graph built for another skeleton is refused.
    const auto other = make_skeleton(chain_data(3));
    CHECK_FALSE(anim.set_graph(single(other, walk_clip(3))));
    CHECK(anim.skeleton() == sk);
    // Changing the skeleton drops the incompatible graph.
    anim.set_skeleton(other);
    CHECK_FALSE(anim.graph_instance().valid());
}

TEST_CASE("Animator: root motion extraction and application") {
    const auto        sk = make_skeleton(chain_data(2));
    AnimatorComponent anim(sk, single(sk, walk_clip(2)));
    RootMotionSettings rm;
    rm.joint = 0;
    anim.set_root_motion(RootMotionMode::Extract, rm);
    anim.tick(0.25f);
    CHECK(near(anim.root_motion_delta().translation, Vec3(0, 0, 0.5f)));
    CHECK(near(anim.local_pose()[0].position, Vec3(0.0f))); // removed from the pose

    anim.set_root_motion(RootMotionMode::Ignore, rm);
    anim.tick(0.25f);
    CHECK(near(anim.root_motion_delta().translation, Vec3(0.0f)));
    CHECK(near(anim.local_pose()[0].position, Vec3(0, 0, 1.0f)));
}

TEST_CASE("Animator: IK chains in model and world space") {
    auto d = chain_data(3);
    d.bind_local[1].rotation = rot_deg(10.0f, kZ);
    const auto        sk = make_skeleton(d);
    AnimatorComponent anim(sk);
    TwoBoneIKChain    chain;
    chain.settings.root = 0;
    chain.settings.mid = 1;
    chain.settings.tip = 2;
    chain.settings.target = Vec3(1, 1, 0);
    const u32 index = anim.add_two_bone_ik(chain);
    CHECK(anim.two_bone_ik_count() == 1);
    anim.tick(0.0f);
    CHECK(near(Vec3(anim.model_pose()[2][3]), Vec3(1, 1, 0), 1e-3f));

    // World-space target through the entity transform (translated by +10 X).
    anim.two_bone_ik(index).space = IKTargetSpace::World;
    anim.two_bone_ik(index).settings.target = Vec3(11, 1, 0);
    const Mat4 world = glm::translate(Mat4(1.0f), Vec3(10, 0, 0));
    anim.tick(0.0f, &world);
    CHECK(near(Vec3(anim.model_pose()[2][3]), Vec3(1, 1, 0), 1e-3f));

    anim.two_bone_ik(index).enabled = false;
    anim.tick(0.0f);
    CHECK_FALSE(near(Vec3(anim.model_pose()[2][3]), Vec3(1, 1, 0), 1e-2f));
    anim.clear_ik();
    CHECK(anim.two_bone_ik_count() == 0);
}

TEST_CASE("AnimationSubsystem: ticks animators and applies root motion to transforms") {
    World      world;
    const auto sk = make_skeleton(chain_data(2));

    const Entity walker = world.create("Walker");
    auto&        anim = world.add<AnimatorComponent>(walker, AnimatorComponent(sk, single(sk, walk_clip(2))));
    RootMotionSettings rm;
    rm.joint = 0;
    anim.set_root_motion(RootMotionMode::ApplyToTransform, rm);

    const Entity idle = world.create("Idle");
    world.add<AnimatorComponent>(idle, AnimatorComponent(sk, single(sk, walk_clip(2))));
    const Entity off = world.create("Off");
    world.add<AnimatorComponent>(off, AnimatorComponent(sk)).set_enabled(false);

    AnimationSubsystem sys;
    CHECK(std::string_view(sys.name()) == "Animation");
    sys.update(world, 0.25f);
    CHECK(sys.animated_count() == 2);
    CHECK(near(scene::local_transform(world, walker).position, Vec3(0, 0, 0.5f)));
    CHECK(world.get<TransformComponent>(walker).dirty);
    CHECK(sys.palette(walker).size() == 2);
    CHECK(sys.palette(world.create("Plain")).empty());

    // Frame-context entry point.
    EngineContext engine;
    engine.world = &world;
    sys.on_startup(engine);
    FrameContext frame;
    frame.engine = &engine;
    frame.time.delta = 0.25f;
    sys.on_update(frame);
    CHECK(near(scene::local_transform(world, walker).position, Vec3(0, 0, 1.0f)));
    sys.on_shutdown();

    // Many animators take the parallel path (inline when no JobSystem workers exist).
    for (int i = 0; i < 16; ++i) {
        world.add<AnimatorComponent>(world.create("Crowd"), AnimatorComponent(sk, single(sk, walk_clip(2))));
    }
    sys.update(world, 0.1f);
    CHECK(sys.animated_count() == 18);
}

TEST_CASE("Animator codec round-trips settings") {
    World world;
    REQUIRE(register_animation_codecs(world));
    const auto   sk = make_skeleton(chain_data(2));
    const Entity e = world.create("Hero");
    auto&        anim = world.add<AnimatorComponent>(e, AnimatorComponent(sk));
    anim.skeleton_asset = AssetId{0x1234, 0x5678};
    anim.graph_asset = AssetId{0xabc, 0xdef};
    anim.set_playback_rate(1.5f);
    anim.set_paused(true);
    RootMotionSettings rm;
    rm.joint = 1;
    rm.extract_vertical = true;
    anim.set_root_motion(RootMotionMode::ApplyToTransform, rm);

    auto text = scene::save_scene_to_string(world);
    REQUIRE(text.has_value());

    World loaded;
    REQUIRE(register_animation_codecs(loaded));
    auto result = scene::load_scene_from_string(loaded, text.value());
    REQUIRE(result.has_value());
    CHECK(result.value().warning_count == 0);
    const Entity le = scene::find_by_name(loaded, "Hero");
    REQUIRE(loaded.has<AnimatorComponent>(le));
    const AnimatorComponent& la = loaded.get<AnimatorComponent>(le);
    CHECK(la.skeleton_asset == anim.skeleton_asset);
    CHECK(la.graph_asset == anim.graph_asset);
    CHECK(la.playback_rate() == doctest::Approx(1.5f));
    CHECK(la.paused());
    CHECK(la.root_motion_mode() == RootMotionMode::ApplyToTransform);
    CHECK(la.root_motion_settings().joint == 1);
    CHECK(la.root_motion_settings().extract_vertical);
}
