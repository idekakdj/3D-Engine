// scene_test_utils.h — helpers shared by the aether.scene tests.
#pragma once

#include "aether/scene/scene.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

namespace scene_test {

using namespace aether;

// Typed null (comparing Entity with entt::null_t inside doctest macros is ambiguous in C++20).
inline constexpr Entity kNull{ entt::null };

// Children of `parent` (roots if null) walked forwards and backwards; asserts both directions,
// the parent back-links and child_count agree. Returns the forward order.
inline std::vector<Entity> checked_children(const World& w, Entity parent) {
    std::vector<Entity> fwd;
    Entity cur = parent == kNull ? scene::first_root(w) : scene::first_child_of(w, parent);
    while (cur != kNull) {
        fwd.push_back(cur);
        CHECK(scene::parent_of(w, cur) == parent);
        cur = scene::next_sibling_of(w, cur);
    }
    std::vector<Entity> bwd;
    cur = parent == kNull ? scene::last_root(w) : scene::last_child_of(w, parent);
    while (cur != kNull) {
        bwd.insert(bwd.begin(), cur);
        cur = scene::prev_sibling_of(w, cur);
    }
    CHECK(fwd == bwd);
    const u32 count =
        parent == kNull ? scene::root_count(w) : scene::child_count_of(w, parent);
    CHECK(count == fwd.size());
    return fwd;
}

inline void check_valid(const World& w) {
    std::string error;
    const bool  ok = scene::validate_hierarchy(w, &error);
    INFO(error);
    CHECK(ok);
}

inline bool mat_near(const Mat4& a, const Mat4& b, f32 eps = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(a[c][r] - b[c][r]) > eps) {
                return false;
            }
        }
    }
    return true;
}

inline bool vec_near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}

} // namespace scene_test
