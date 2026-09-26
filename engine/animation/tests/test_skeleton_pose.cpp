// test_skeleton_pose.cpp — Skeleton, model space, palette, blending, additive, masks, math.
#include "aether/animation/pose.h"
#include "anim_test_utils.h"

#include <vector>

using namespace anim_test;

TEST_CASE("Skeleton: 3-joint chain model space and skinning palette") {
    assets::SkeletonData d;
    d.joint_names = {"root", "mid", "tip"};
    d.parents = {-1, 0, 1};
    d.bind_local.resize(3);
    d.bind_local[0].position = Vec3(1.0f, 0.0f, 0.0f);
    d.bind_local[1].position = Vec3(0.0f, 2.0f, 0.0f);
    d.bind_local[1].rotation = rot_deg(90.0f, kZ);
    d.bind_local[2].position = Vec3(0.0f, 3.0f, 0.0f);
    const auto sk = make_skeleton(d);

    REQUIRE(sk->joint_count() == 3);
    CHECK(sk->find_joint("mid") == 1);
    CHECK(sk->find_joint("nope") == kInvalidJoint);
    CHECK(sk->is_ancestor(0, 2));
    CHECK_FALSE(sk->is_ancestor(2, 0));

    // Bind model positions: root (1,0,0); mid (1,2,0); tip = mid + Rz90*(0,3,0) = (-2,2,0).
    std::vector<Mat4> model(3), palette(3);
    local_to_model(*sk, sk->bind_pose(), model);
    CHECK(near(Vec3(model[0][3]), Vec3(1.0f, 0.0f, 0.0f)));
    CHECK(near(Vec3(model[1][3]), Vec3(1.0f, 2.0f, 0.0f)));
    CHECK(near(Vec3(model[2][3]), Vec3(-2.0f, 2.0f, 0.0f)));
    CHECK(near(Vec3(model[2] * Vec4(1.0f, 0.0f, 0.0f, 0.0f)), kY)); // tip x-axis -> +Y
    CHECK(near(model_transform(*sk, sk->bind_pose(), 2).position, Vec3(-2.0f, 2.0f, 0.0f)));

    // Inverse binds were computed from the bind pose -> identity palette at bind.
    compute_skinning_palette(model, sk->inverse_bind(), palette);
    for (const Mat4& m : palette) {
        CHECK(near(m, Mat4(1.0f)));
    }

    // Rotate the root 90 degrees about +Y: tip -> (1,0,0) + Ry90*((0,2,0) + Rz90*(0,3,0)) = (1,2,3).
    Pose pose(*sk);
    pose[0].rotation = rot_deg(90.0f, kY);
    local_to_model(*sk, pose.local(), model);
    compute_skinning_palette(model, sk->inverse_bind(), palette);
    CHECK(near(Vec3(model[2][3]), Vec3(1.0f, 2.0f, 3.0f)));
    // A vertex bound at the tip's bind position follows the tip.
    const Vec4 skinned = palette[2] * Vec4(-2.0f, 2.0f, 0.0f, 1.0f);
    CHECK(near(Vec3(skinned), Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(near(palette[1], model[1] * sk->inverse_bind()[1]));
}

TEST_CASE("Skeleton: validation") {
    auto d = chain_data(3);
    d.parents[1] = 1; // self-parent: not topologically sorted
    CHECK_FALSE(Skeleton::create(d).has_value());

    d = chain_data(3);
    d.parents[1] = 2; // forward reference
    CHECK_FALSE(Skeleton::create(d).has_value());

    d = chain_data(3);
    d.bind_local.pop_back();
    CHECK_FALSE(Skeleton::create(d).has_value());

    d = chain_data(3);
    d.inverse_bind.resize(2);
    CHECK_FALSE(Skeleton::create(d).has_value());

    CHECK_FALSE(Skeleton::create(assets::SkeletonData{}).has_value());

    d = chain_data(2);
    d.joint_names = {"", "b"}; // unnamed joints get generated names
    const auto sk = make_skeleton(d);
    CHECK(sk->joint_name(0) == "joint_0");
    CHECK(sk->find_joint("b") == 1);
}

TEST_CASE("Pose: blend weights 0, 0.5, 1 with hemisphere correction") {
    const Transform a = make_transform(Vec3(0.0f), quat_identity(), Vec3(1.0f));
    Transform       b = make_transform(Vec3(2.0f, 4.0f, 6.0f), rot_deg(90.0f, kY), Vec3(3.0f));
    std::vector<Transform> pa{a}, pb{b}, out(1);

    blend_poses(pa, pb, 0.0f, out);
    CHECK(near(out[0].position, a.position));
    CHECK(near(out[0].rotation, a.rotation));
    blend_poses(pa, pb, 1.0f, out);
    CHECK(near(out[0].position, b.position));
    CHECK(near(out[0].rotation, b.rotation));
    CHECK(near(out[0].scale, b.scale));
    blend_poses(pa, pb, 0.5f, out);
    CHECK(near(out[0].position, Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(near(out[0].scale, Vec3(2.0f)));
    CHECK(near(out[0].rotation, rot_deg(45.0f, kY)));

    // Same rotation stored in the opposite hemisphere: result must not take the long way.
    pb[0].rotation = make_quat(-b.rotation.w, -b.rotation.x, -b.rotation.y, -b.rotation.z);
    blend_poses(pa, pb, 0.5f, out);
    CHECK(near(out[0].rotation, rot_deg(45.0f, kY)));

    // In-place (out aliases a).
    blend_poses(pa, pb, 0.5f, pa);
    CHECK(near(pa[0].position, Vec3(1.0f, 2.0f, 3.0f)));
}

TEST_CASE("Pose: additive identity, round trip and weighting") {
    const Transform ref = make_transform(Vec3(1.0f, 2.0f, 3.0f), rot_deg(30.0f, kX), Vec3(2.0f));
    const Transform pose = make_transform(Vec3(-1.0f, 0.5f, 4.0f), rot_deg(70.0f, kY), Vec3(3.0f));

    const Transform identity = make_additive(pose, pose);
    CHECK(near(identity.position, Vec3(0.0f)));
    CHECK(near(identity.rotation, quat_identity()));
    CHECK(near(identity.scale, Vec3(1.0f)));
    const Transform unchanged = apply_additive(ref, identity, 0.7f);
    CHECK(near(unchanged.position, ref.position));
    CHECK(near(unchanged.rotation, ref.rotation));
    CHECK(near(unchanged.scale, ref.scale));

    const Transform back = apply_additive(ref, make_additive(pose, ref), 1.0f);
    CHECK(near(back.position, pose.position));
    CHECK(near(back.rotation, pose.rotation));
    CHECK(near(back.scale, pose.scale));

    // delta = +2 x, 90 deg about Y, x2 scale; half weight on a rotated base.
    const Transform delta = make_transform(Vec3(2.0f, 0.0f, 0.0f), rot_deg(90.0f, kY), Vec3(2.0f));
    const Transform base = make_transform(Vec3(1.0f), rot_deg(90.0f, kX), Vec3(1.0f));
    std::vector<Transform> bases{base}, deltas{delta}, out(1);
    apply_additive(bases, deltas, 0.5f, out);
    CHECK(near(out[0].position, Vec3(2.0f, 1.0f, 1.0f)));
    CHECK(near(out[0].rotation, rot_deg(45.0f, kY) * rot_deg(90.0f, kX)));
    CHECK(near(out[0].scale, Vec3(1.5f)));
    apply_additive(bases, deltas, 0.0f, out);
    CHECK(near(out[0].position, base.position));
}

TEST_CASE("Pose: bone masks") {
    const auto sk = make_skeleton(chain_data(4));
    const BoneMask mask = BoneMask::from_branch(*sk, "j1");
    REQUIRE(mask.joint_count() == 4);
    CHECK(mask.weight(0) == 0.0f);
    CHECK(mask.weight(1) == 1.0f);
    CHECK(mask.weight(3) == 1.0f);
    CHECK(BoneMask::from_branch(*sk, "missing").weight(2) == 0.0f);

    std::vector<Transform> a(4), b(4), out(4);
    for (auto& t : b) {
        t.position = Vec3(10.0f, 0.0f, 0.0f);
    }
    blend_poses_masked(a, b, 1.0f, mask, out);
    CHECK(near(out[0].position, Vec3(0.0f)));
    CHECK(near(out[1].position, Vec3(10.0f, 0.0f, 0.0f)));
    CHECK(near(out[3].position, Vec3(10.0f, 0.0f, 0.0f)));
    blend_poses_masked(a, b, 0.5f, mask, out);
    CHECK(near(out[2].position, Vec3(5.0f, 0.0f, 0.0f)));
    CHECK(near(out[0].position, Vec3(0.0f)));

    // Masked additive: only the branch receives the delta.
    std::vector<Transform> delta(4);
    for (auto& t : delta) {
        t.position = Vec3(0.0f, 1.0f, 0.0f);
    }
    apply_additive_masked(a, delta, 1.0f, mask, out);
    CHECK(near(out[0].position, Vec3(0.0f)));
    CHECK(near(out[2].position, Vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("anim_math: slerp, rotation_between, twist, TRS inverse") {
    CHECK(near(slerp(quat_identity(), rot_deg(90.0f, kY), 0.25f), rot_deg(22.5f, kY)));
    CHECK(near(rotation_between(kX, kY), rot_deg(90.0f, kZ)));
    CHECK(near(rotation_between(kX, -kX) * kX, -kX));
    CHECK(near(twist_angle(rot_deg(30.0f, kY) * rot_deg(20.0f, kX), kY), 30.0f * kDeg2Rad));
    CHECK(near(twist(rot_deg(-50.0f, kY), kY), rot_deg(-50.0f, kY)));

    const Transform t = make_transform(Vec3(1.0f, 2.0f, 3.0f), rot_deg(40.0f, kZ), Vec3(2.0f));
    const Transform i = compose(inverse(t), t);
    CHECK(near(i.position, Vec3(0.0f)));
    CHECK(near(i.rotation, quat_identity()));
    CHECK(near(i.scale, Vec3(1.0f)));
    CHECK(near(to_mat4(t), t.to_matrix()));
    CHECK(near(quat_from_xyzw(quat_to_xyzw(t.rotation)), t.rotation));
}
