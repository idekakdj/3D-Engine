// test_snapping.cpp — snapping math and group (pivot) transforms.
#include "aether/editor/snapping.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps))); }
Mat4 translation(const Vec3& t) { return glm::translate(Mat4(1.0f), t); }
} // namespace

TEST_CASE("Snapping: values and positions") {
    CHECK(near(snap_value(0.74f, 0.5f), 0.5f));
    CHECK(near(snap_value(0.76f, 0.5f), 1.0f));
    CHECK(near(snap_value(-1.26f, 0.25f), -1.25f));
    CHECK(snap_value(0.3f, 0.0f) == 0.3f);  // disabled
    CHECK(snap_value(0.3f, -1.0f) == 0.3f); // disabled
    CHECK(!std::signbit(snap_value(-0.1f, 1.0f))); // no -0
    CHECK(near(snap_position(Vec3(0.3f, 2.2f, -0.1f), 0.5f), Vec3(0.5f, 2.0f, 0.0f)));
}

TEST_CASE("Snapping: rotation and scale") {
    const Quat q = glm::angleAxis(glm::radians(37.0f), Vec3(0, 1, 0));
    const Quat s = snap_rotation(q, 15.0f);
    CHECK(near(std::abs(glm::dot(s, glm::angleAxis(glm::radians(30.0f), Vec3(0, 1, 0)))), 1.0f));
    CHECK(near(std::abs(glm::dot(snap_rotation(q, 0.0f), q)), 1.0f)); // disabled

    CHECK(near(snap_scale(Vec3(1.26f, 0.02f, -0.74f), 0.25f), Vec3(1.25f, 0.25f, -0.75f))); // never collapses to 0
    CHECK(near(snap_scale(Vec3(1.26f), 0.0f), Vec3(1.26f)));
}

TEST_CASE("Snapping: matrix translation keeps rotation and scale") {
    Mat4       m = glm::rotate(Mat4(1.0f), glm::radians(30.0f), Vec3(0, 0, 1));
    m            = glm::scale(m, Vec3(2.0f));
    m[3]         = Vec4(1.2f, -0.4f, 3.9f, 1.0f);
    const Mat4 s = snap_translation(m, 1.0f);
    CHECK(near(Vec3(s[3]), Vec3(1.0f, 0.0f, 4.0f)));
    for (int c = 0; c < 3; ++c) {
        CHECK(near(Vec3(s[c]), Vec3(m[c])));
    }
}

TEST_CASE("Group pivot: bounds centre, primary and orientation") {
    const Quat              r = glm::angleAxis(glm::radians(90.0f), Vec3(0, 1, 0));
    const std::vector<Mat4> worlds{ translation(Vec3(-2, 0, 0)), translation(Vec3(2, 2, 0)),
                                    translation(Vec3(0, 1, 4)) * glm::mat4_cast(r) * glm::scale(Mat4(1.0f), Vec3(3.0f)) };
    const Mat4 center = selection_pivot(worlds, 2, PivotMode::BoundsCenter, false);
    CHECK(near(Vec3(center[3]), Vec3(0, 1, 2)));
    CHECK(near(Vec3(center[0]), Vec3(1, 0, 0)));

    const Mat4 primary = selection_pivot(worlds, 2, PivotMode::Primary, true);
    CHECK(near(Vec3(primary[3]), Vec3(0, 1, 4)));
    // Local orientation: the primary's rotation, scale removed.
    CHECK(near(Vec3(primary[0]), r * Vec3(1, 0, 0)));
    CHECK(near(glm::length(Vec3(primary[2])), 1.0f));
    CHECK(selection_pivot({}, 0, PivotMode::BoundsCenter, false) == Mat4(1.0f));
}

TEST_CASE("Group pivot: delta moves, rotates and scales about the pivot") {
    const Mat4 pivot = translation(Vec3(1, 0, 0));
    const Mat4 a     = translation(Vec3(0, 0, 0));
    const Mat4 b     = translation(Vec3(2, 0, 0));

    // Translate: both move by the pivot's offset.
    const Mat4 moved = translation(Vec3(1, 3, 0));
    CHECK(near(Vec3(apply_pivot_delta(pivot, moved, a)[3]), Vec3(0, 3, 0)));
    CHECK(near(Vec3(apply_pivot_delta(pivot, moved, b)[3]), Vec3(2, 3, 0)));

    // Rotate 180 degrees about +Y at the pivot: the two swap places and turn around.
    const Mat4 turned = pivot * glm::rotate(Mat4(1.0f), glm::radians(180.0f), Vec3(0, 1, 0));
    const Mat4 ra     = apply_pivot_delta(pivot, turned, a);
    CHECK(near(Vec3(ra[3]), Vec3(2, 0, 0)));
    CHECK(near(Vec3(ra[0]), Vec3(-1, 0, 0)));

    // Scale x2 at the pivot: distances to the pivot double, entity scale doubles.
    const Mat4 scaled = pivot * glm::scale(Mat4(1.0f), Vec3(2.0f));
    const Mat4 sb     = apply_pivot_delta(pivot, scaled, b);
    CHECK(near(Vec3(sb[3]), Vec3(3, 0, 0)));
    CHECK(near(glm::length(Vec3(sb[0])), 2.0f));

    // Identity delta is exact.
    CHECK(near(Vec3(apply_pivot_delta(pivot, pivot, b)[3]), Vec3(2, 0, 0)));
}
