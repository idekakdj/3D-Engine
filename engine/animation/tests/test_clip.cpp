// test_clip.cpp — AnimationClip sampling (step/linear/cubic), validation, wrap modes, notifies.
#include "anim_test_utils.h"

#include <vector>

using namespace anim_test;

namespace {

assets::AnimationClipData translation_clip(Interpolation interp, std::initializer_list<f32> times,
                                           std::initializer_list<Vec4> values) {
    assets::AnimationClipData d;
    d.name = "t";
    d.channels.push_back(channel(0, AnimPath::Translation, interp, times, values));
    return d;
}

Vec3 sample_position(const AnimationClip& clip, f32 t, ClipCursor* cursor = nullptr) {
    std::vector<Transform> pose(1);
    clip.sample(t, pose, cursor);
    return pose[0].position;
}

} // namespace

TEST_CASE("Clip: linear translation, holds before the first and after the last key") {
    const AnimationClip clip = make_clip_value(
        translation_clip(Interpolation::Linear, {0.5f, 1.5f}, {Vec4(0, 0, 0, 0), Vec4(10, 0, 0, 0)}), 1);
    CHECK(clip.duration() == doctest::Approx(1.5f)); // last key time when duration is unset
    CHECK(clip.track_count() == 1);
    CHECK(clip.required_joint_count() == 1);
    CHECK(clip.animates(0));
    CHECK_FALSE(clip.animates(1));
    CHECK(near(sample_position(clip, 0.0f), Vec3(0.0f)));
    CHECK(near(sample_position(clip, 0.5f), Vec3(0.0f)));
    CHECK(near(sample_position(clip, 1.0f), Vec3(5, 0, 0)));
    CHECK(near(sample_position(clip, 1.5f), Vec3(10, 0, 0)));
    CHECK(near(sample_position(clip, 9.0f), Vec3(10, 0, 0)));
}

TEST_CASE("Clip: step interpolation") {
    const AnimationClip clip = make_clip_value(
        translation_clip(Interpolation::Step, {0.0f, 1.0f, 2.0f},
                         {Vec4(1, 0, 0, 0), Vec4(2, 0, 0, 0), Vec4(3, 0, 0, 0)}),
        1);
    CHECK(near(sample_position(clip, 0.25f), Vec3(1, 0, 0)));
    CHECK(near(sample_position(clip, 0.99f), Vec3(1, 0, 0)));
    CHECK(near(sample_position(clip, 1.0f), Vec3(2, 0, 0)));
    CHECK(near(sample_position(clip, 1.5f), Vec3(2, 0, 0)));
    CHECK(near(sample_position(clip, 2.0f), Vec3(3, 0, 0)));
}

TEST_CASE("Clip: cubic spline (glTF Hermite)") {
    // Keys (in-tangent, value, out-tangent): zero tangents give a smoothstep-shaped curve.
    const AnimationClip flat = make_clip_value(
        translation_clip(Interpolation::CubicSpline, {0.0f, 1.0f},
                         {Vec4(0), Vec4(0), Vec4(0), Vec4(0), Vec4(4, 0, 0, 0), Vec4(0)}),
        1);
    CHECK(near(sample_position(flat, 0.5f), Vec3(2, 0, 0)));
    CHECK(near(sample_position(flat, 0.25f), Vec3(4.0f * (3.0f * 0.0625f - 2.0f * 0.015625f), 0, 0)));
    CHECK(near(sample_position(flat, 1.0f), Vec3(4, 0, 0)));

    // Tangents matching the slope reproduce a straight line (tangents are scaled by the interval).
    const AnimationClip line = make_clip_value(
        translation_clip(Interpolation::CubicSpline, {0.0f, 2.0f},
                         {Vec4(1, 0, 0, 0), Vec4(0), Vec4(1, 0, 0, 0),
                          Vec4(1, 0, 0, 0), Vec4(2, 0, 0, 0), Vec4(1, 0, 0, 0)}),
        1);
    CHECK(near(sample_position(line, 0.5f), Vec3(0.5f, 0, 0)));
    CHECK(near(sample_position(line, 1.5f), Vec3(1.5f, 0, 0)));
}

TEST_CASE("Clip: rotation slerp takes the shortest arc") {
    const Quat q0 = rot_deg(0.0f, kY);
    const Quat q1 = rot_deg(90.0f, kY);
    assets::AnimationClipData d;
    d.channels.push_back(channel(0, AnimPath::Rotation, Interpolation::Linear, {0.0f, 1.0f},
                                 {quat_to_xyzw(q0), quat_to_xyzw(q1)}));
    const AnimationClip clip = make_clip_value(d, 1);
    const Transform     mid  = clip.sample_joint(0, 0.5f, Transform{});
    CHECK(near(mid.rotation, rot_deg(45.0f, kY)));

    // The second key stored in the opposite hemisphere (-q) must not spin the long way round.
    assets::AnimationClipData neg;
    neg.channels.push_back(channel(0, AnimPath::Rotation, Interpolation::Linear, {0.0f, 1.0f},
                                   {quat_to_xyzw(q0), -quat_to_xyzw(q1)}));
    const AnimationClip neg_clip = make_clip_value(neg, 1);
    CHECK(near(neg_clip.sample_joint(0, 0.5f, Transform{}).rotation, rot_deg(45.0f, kY)));
}

TEST_CASE("Clip: scale track and untouched paths / joints") {
    assets::AnimationClipData d;
    d.duration = 4.0f;
    d.channels.push_back(channel(1, AnimPath::Scale, Interpolation::Linear, {0.0f, 1.0f},
                                 {Vec4(1, 1, 1, 0), Vec4(3, 3, 3, 0)}));
    const AnimationClip clip = make_clip_value(d, 2);
    CHECK(clip.duration() == doctest::Approx(4.0f)); // explicit duration wins
    CHECK(clip.required_joint_count() == 2);

    std::vector<Transform> pose(2);
    pose[0].position = Vec3(7, 7, 7);
    pose[1].position = Vec3(1, 2, 3);
    clip.sample(0.5f, pose);
    CHECK(near(pose[0].position, Vec3(7, 7, 7))); // joint 0 not animated
    CHECK(near(pose[1].position, Vec3(1, 2, 3))); // translation not animated
    CHECK(near(pose[1].scale, Vec3(2.0f)));

    // sample_pose starts from the bind pose.
    const auto sk = make_skeleton(chain_data(2, 5.0f));
    std::vector<Transform> full(2);
    clip.sample_pose(0.0f, *sk, full);
    CHECK(near(full[1].position, Vec3(0, 5, 0)));
    CHECK(near(full[1].scale, Vec3(1.0f)));
}

TEST_CASE("Clip: validation") {
    // Joint out of range.
    CHECK_FALSE(AnimationClip::create(
                    translation_clip(Interpolation::Linear, {0.0f}, {Vec4(0)}), 0)
                    .has_value());
    // Decreasing times.
    CHECK_FALSE(AnimationClip::create(translation_clip(Interpolation::Linear, {1.0f, 0.5f},
                                                       {Vec4(0), Vec4(1)}),
                                      1)
                    .has_value());
    // Key/value count mismatch.
    CHECK_FALSE(AnimationClip::create(
                    translation_clip(Interpolation::Linear, {0.0f, 1.0f}, {Vec4(0)}), 1)
                    .has_value());
    // Cubic spline needs three values per key.
    CHECK_FALSE(AnimationClip::create(translation_clip(Interpolation::CubicSpline, {0.0f, 1.0f},
                                                       {Vec4(0), Vec4(1)}),
                                      1)
                    .has_value());

    // Empty channels and duplicates are skipped, not fatal.
    assets::AnimationClipData d;
    d.channels.push_back(channel(0, AnimPath::Translation, Interpolation::Linear, {}, {}));
    d.channels.push_back(channel(0, AnimPath::Rotation, Interpolation::Linear, {0.0f},
                                 {quat_to_xyzw(quat_identity())}));
    d.channels.push_back(channel(0, AnimPath::Rotation, Interpolation::Linear, {0.0f},
                                 {quat_to_xyzw(rot_deg(90.0f, kX))}));
    auto clip = AnimationClip::create(d, 1);
    REQUIRE(clip.has_value());
    CHECK(clip->track_count() == 1);
    CHECK(near(clip->sample_joint(0, 0.0f, Transform{}).rotation, quat_identity()));

    // Against a skeleton.
    const auto sk = make_skeleton(chain_data(3));
    CHECK(AnimationClip::create(translation_clip(Interpolation::Linear, {0.0f}, {Vec4(0)}), *sk)
              .has_value());
}

TEST_CASE("Clip: cursor sampling matches uncached sampling") {
    std::initializer_list<f32> times = {0.0f, 0.1f, 0.2f, 0.4f, 0.8f, 1.0f};
    const AnimationClip        clip  = make_clip_value(
        translation_clip(Interpolation::Linear, times,
                         {Vec4(0), Vec4(1, 0, 0, 0), Vec4(4, 0, 0, 0), Vec4(2, 0, 0, 0),
                          Vec4(8, 0, 0, 0), Vec4(3, 0, 0, 0)}),
        1);
    ClipCursor cursor;
    clip.prepare_cursor(cursor);
    // Forward, then a jump backwards (loop wrap), then forward again.
    for (const f32 t : {0.0f, 0.05f, 0.15f, 0.3f, 0.9f, 0.95f, 0.02f, 0.5f, 1.0f, 0.7f}) {
        CHECK(near(sample_position(clip, t, &cursor), sample_position(clip, t)));
    }
    cursor.reset();
    CHECK(near(sample_position(clip, 0.35f, &cursor), sample_position(clip, 0.35f)));
}

TEST_CASE("wrap_time") {
    CHECK(wrap_time(0.25, 1.0f, WrapMode::Loop) == doctest::Approx(0.25f));
    CHECK(wrap_time(2.5, 1.0f, WrapMode::Loop) == doctest::Approx(0.5f));
    CHECK(wrap_time(-0.25, 1.0f, WrapMode::Loop) == doctest::Approx(0.75f));
    CHECK(wrap_time(2.5, 1.0f, WrapMode::Clamp) == doctest::Approx(1.0f));
    CHECK(wrap_time(-1.0, 1.0f, WrapMode::Clamp) == doctest::Approx(0.0f));
    CHECK(wrap_time(0.25, 1.0f, WrapMode::PingPong) == doctest::Approx(0.25f));
    CHECK(wrap_time(1.25, 1.0f, WrapMode::PingPong) == doctest::Approx(0.75f));
    CHECK(wrap_time(2.25, 1.0f, WrapMode::PingPong) == doctest::Approx(0.25f));
    CHECK(wrap_time(5.0, 0.0f, WrapMode::Loop) == doctest::Approx(0.0f)); // zero-length clip
}

TEST_CASE("Notifies: half-open crossing rules") {
    AnimationClip clip = make_clip_value(
        translation_clip(Interpolation::Linear, {0.0f, 1.0f}, {Vec4(0), Vec4(1)}), 1);
    clip.add_notify("step_r", 0.75f);
    clip.add_notify("step_l", 0.25f);
    clip.add_notify("late", 5.0f); // clamped into [0, duration]
    REQUIRE(clip.notifies().size() == 3);
    CHECK(clip.notifies()[0].name == "step_l"); // time-sorted
    CHECK(clip.notifies()[2].time == doctest::Approx(1.0f));

    std::vector<const AnimNotify*> out;
    SUBCASE("forward [from, to)") {
        // Loop: "late" (at duration) fires as the first cycle begins; step_l (0.25) is excluded.
        CHECK(collect_notifies(clip, 0.0, 0.25, WrapMode::Loop, out) == 1);
        CHECK(out.back()->name == "late");
        CHECK(collect_notifies(clip, 0.0, 0.25, WrapMode::Clamp, out) == 0);
        CHECK(collect_notifies(clip, 0.25, 0.5, WrapMode::Loop, out) == 1);
        CHECK(out.back()->name == "step_l");
    }
    SUBCASE("sliced updates fire each notify exactly once") {
        u32 total = 0;
        for (f64 t = 0.0; t < 0.999; t += 0.05) {
            total += collect_notifies(clip, t, t + 0.05, WrapMode::Clamp, out);
        }
        CHECK(total == 3); // step_l, step_r and "late" at the clamped end
    }
    SUBCASE("loop wraps fire every cycle, in playback order") {
        // [0.5, 2.5): step_r 0.75, late 1.0, step_l 1.25, step_r 1.75, late 2.0, step_l 2.25.
        CHECK(collect_notifies(clip, 0.5, 2.5, WrapMode::Loop, out) == 6);
        REQUIRE(out.size() == 6);
        CHECK(out[0]->name == "step_r");
        CHECK(out[1]->name == "late"); // a notify at `duration` fires as the next cycle begins
        CHECK(out[2]->name == "step_l");
    }
    SUBCASE("backward playback uses (to, from]") {
        CHECK(collect_notifies(clip, 0.75, 0.25, WrapMode::Clamp, out) == 1);
        CHECK(out.back()->name == "step_r");
    }
    SUBCASE("ping-pong fires on both legs") {
        CHECK(collect_notifies(clip, 0.0, 2.0, WrapMode::PingPong, out) >= 4);
    }
}
