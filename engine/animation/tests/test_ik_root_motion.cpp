// test_ik_root_motion.cpp — two-bone IK, look-at, root motion extraction and application.
#include "aether/animation/ik.h"
#include "aether/animation/pose.h"
#include "aether/animation/root_motion.h"
#include "anim_test_utils.h"

#include <vector>

using namespace anim_test;

namespace {

// A 3-joint arm along +Y with a slight bend at the elbow (so the bend plane is defined).
std::shared_ptr<const Skeleton> bent_arm() {
    auto d = chain_data(3);
    d.bind_local[1].rotation = rot_deg(10.0f, kZ);
    return make_skeleton(d);
}

Vec3 model_position(const Skeleton& sk, std::span<const Transform> local, u32 joint) {
    return model_transform(sk, local, joint).position;
}

} // namespace

TEST_CASE("Two-bone IK: reaches reachable targets and preserves bone lengths") {
    const auto sk = bent_arm();
    Pose       pose(*sk);
    const f32  upper = length(model_position(*sk, pose.local(), 1) - model_position(*sk, pose.local(), 0));
    const f32  lower = length(model_position(*sk, pose.local(), 2) - model_position(*sk, pose.local(), 1));

    for (const Vec3 target : {Vec3(1, 1, 0), Vec3(-0.5f, 1.2f, 0.3f), Vec3(0.2f, 0.5f, -1.0f)}) {
        Pose              p(*sk);
        TwoBoneIKSettings s;
        s.root = 0;
        s.mid = 1;
        s.tip = 2;
        s.target = target;
        REQUIRE(solve_two_bone_ik(*sk, p.local(), s));
        CHECK(near(model_position(*sk, p.local(), 2), target, 1e-3f));
        CHECK(near(length(model_position(*sk, p.local(), 1) - model_position(*sk, p.local(), 0)), upper, 1e-3f));
        CHECK(near(length(model_position(*sk, p.local(), 2) - model_position(*sk, p.local(), 1)), lower, 1e-3f));
        // Positions are untouched; only rotations are written.
        CHECK(near(p[1].position, pose[1].position));
    }
}

TEST_CASE("Two-bone IK: unreachable targets extend the chain toward the target") {
    const auto        sk = bent_arm();
    Pose              p(*sk);
    TwoBoneIKSettings s;
    s.root = 0;
    s.mid = 1;
    s.tip = 2;
    s.target = Vec3(10, 0, 0);
    REQUIRE(solve_two_bone_ik(*sk, p.local(), s));
    const Vec3 tip = model_position(*sk, p.local(), 2);
    CHECK(near(tip, Vec3(2, 0, 0), 1e-2f));
}

TEST_CASE("Two-bone IK: weight, pole and invalid chains") {
    const auto sk = bent_arm();
    const Pose bind(*sk);

    TwoBoneIKSettings s;
    s.root = 0;
    s.mid = 1;
    s.tip = 2;
    s.target = Vec3(1, 1, 0);

    Pose zero(*sk);
    s.weight = 0.0f;
    solve_two_bone_ik(*sk, zero.local(), s);
    CHECK(near(model_position(*sk, zero.local(), 2), model_position(*sk, bind.local(), 2)));

    // With a pole the elbow bends toward it.
    s.weight = 1.0f;
    s.use_pole = true;
    for (const f32 side : {1.0f, -1.0f}) {
        Pose p(*sk);
        s.target = Vec3(0, 1.5f, 0);
        s.pole = Vec3(0, 0.75f, side * 5.0f);
        REQUIRE(solve_two_bone_ik(*sk, p.local(), s));
        CHECK(near(model_position(*sk, p.local(), 2), s.target, 1e-3f));
        CHECK(model_position(*sk, p.local(), 1).z * side > 0.1f);
    }

    // Target rotation also orients the tip.
    {
        Pose p(*sk);
        s.use_pole = false;
        s.target = Vec3(1, 1, 0);
        s.use_target_rotation = true;
        s.target_rotation = rot_deg(30.0f, kX);
        REQUIRE(solve_two_bone_ik(*sk, p.local(), s));
        CHECK(near(model_transform(*sk, p.local(), 2).rotation, s.target_rotation, 1e-3f));
    }

    Pose              bad(*sk);
    TwoBoneIKSettings wrong = s;
    wrong.root = 2;
    wrong.mid = 1;
    wrong.tip = 0; // not a descending chain
    CHECK_FALSE(solve_two_bone_ik(*sk, bad.local(), wrong));
    wrong.tip = kInvalidJoint;
    CHECK_FALSE(solve_two_bone_ik(*sk, bad.local(), wrong));
    for (u32 j = 0; j < 3; ++j) {
        CHECK(near(bad[j].rotation, bind[j].rotation));
    }
}

TEST_CASE("Look-at: points the forward axis, respects max angle and weight") {
    const auto     sk = make_skeleton(chain_data(2));
    LookAtSettings s;
    s.joint = 1;
    s.forward_axis = kZ;
    s.target = Vec3(5, 1, 0); // joint 1 sits at (0, 1, 0): direction +X
    {
        Pose p(*sk);
        REQUIRE(solve_look_at(*sk, p.local(), s));
        const Quat r = model_transform(*sk, p.local(), 1).rotation;
        CHECK(near(r * kZ, kX, 1e-3f));
    }
    {
        Pose p(*sk);
        s.max_angle = 45.0f * kDeg2Rad;
        REQUIRE(solve_look_at(*sk, p.local(), s));
        const Vec3 fwd = model_transform(*sk, p.local(), 1).rotation * kZ;
        CHECK(std::acos(std::clamp(dot(fwd, kZ), -1.0f, 1.0f)) == doctest::Approx(45.0f * kDeg2Rad).epsilon(1e-3));
    }
    {
        Pose p(*sk);
        s.max_angle = kPi;
        s.weight = 0.0f;
        solve_look_at(*sk, p.local(), s);
        CHECK(near(model_transform(*sk, p.local(), 1).rotation * kZ, kZ, 1e-4f));
    }
}

TEST_CASE("Root motion: motion frames, combine and relative") {
    RootMotionSettings s;
    s.joint = 0;
    const Transform t = make_transform(Vec3(1, 2, 3), rot_deg(90.0f, kY) * rot_deg(20.0f, kX), Vec3(1.0f));
    const RootMotionDelta m = extract_motion(t, s);
    CHECK(near(m.translation, Vec3(1, 0, 3))); // vertical not extracted by default
    CHECK(m.yaw == doctest::Approx(kHalfPi).epsilon(1e-3));

    s.extract_vertical = true;
    s.extract_yaw = false;
    const RootMotionDelta v = extract_motion(t, s);
    CHECK(near(v.translation, Vec3(1, 2, 3)));
    CHECK(v.yaw == doctest::Approx(0.0f));

    RootMotionDelta a;
    a.translation = Vec3(1, 0, 0);
    a.yaw = kHalfPi;
    RootMotionDelta b;
    b.translation = Vec3(0, 0, 2);
    b.yaw = 0.25f;
    const RootMotionDelta ab = combine(a, b);
    // b is expressed in a's frame: a yaw of +90 deg maps local +Z to world +X.
    CHECK(near(ab.translation, Vec3(1, 0, 0) + rotate_yaw(kHalfPi, Vec3(0, 0, 2))));
    CHECK(ab.yaw == doctest::Approx(kHalfPi + 0.25f));
    const RootMotionDelta back = relative(a, ab);
    CHECK(near(back.translation, b.translation));
    CHECK(back.yaw == doctest::Approx(b.yaw));
    CHECK(near(a.rotation(), yaw_quat(kHalfPi)));
}

TEST_CASE("Root motion: clip extraction across loop wraps, removal and application") {
    const auto sk = make_skeleton(chain_data(2));
    assets::AnimationClipData d;
    d.name = "walk";
    d.channels.push_back(channel(0, AnimPath::Translation, Interpolation::Linear, {0.0f, 1.0f},
                                 {Vec4(0, 0.1f, 0, 0), Vec4(0, 0.1f, 2, 0)}));
    const AnimationClip clip = make_clip_value(d, 2);
    RootMotionSettings  s;
    s.joint = 0;

    const RootMotionDelta half = extract_clip_root_motion(clip, *sk, s, 0.0, 0.5, WrapMode::Loop);
    CHECK(near(half.translation, Vec3(0, 0, 1)));
    // Across the loop point: never a jump back.
    const RootMotionDelta wrap = extract_clip_root_motion(clip, *sk, s, 0.75, 1.25, WrapMode::Loop);
    CHECK(near(wrap.translation, Vec3(0, 0, 1)));
    // Clamp: stops at the end.
    const RootMotionDelta clamp = extract_clip_root_motion(clip, *sk, s, 0.75, 3.0, WrapMode::Clamp);
    CHECK(near(clamp.translation, Vec3(0, 0, 0.5f)));

    std::vector<Transform> pose(2);
    clip.sample_pose(0.5f, *sk, pose);
    remove_root_motion(*sk, s, pose);
    CHECK(near(pose[0].position, Vec3(0, 0.1f, 0))); // height residual kept

    Transform       entity = make_transform(Vec3(5, 0, 0), yaw_quat(kHalfPi), Vec3(1.0f));
    RootMotionDelta step;
    step.translation = Vec3(0, 0, 1);
    apply_root_motion(entity, step);
    CHECK(near(entity.position, Vec3(5, 0, 0) + rotate_yaw(kHalfPi, Vec3(0, 0, 1))));
    step = {};
    step.yaw = kHalfPi;
    apply_root_motion(entity, step);
    CHECK(near(entity.rotation, yaw_quat(kPi)));
}
