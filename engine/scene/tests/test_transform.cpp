// test_transform.cpp — transform propagation, dirty-subtree-only updates, 100k-entity
// hierarchies, allocation-free steady state, keep-world reparenting, decomposition.
#include "scene_test_utils.h"

#include <atomic>
#include <cstdlib>
#include <new>

// ---- global allocation counter (replaceable operator new/delete for this test exe) -----------
namespace {
std::atomic<long long> g_allocations{ 0 };
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

using namespace aether;
using namespace scene_test;

namespace {

Transform trs(Vec3 p, Quat r, Vec3 s) {
    Transform t;
    t.position = p;
    t.rotation = r;
    t.scale    = s;
    return t;
}

Quat axis_angle(f32 deg, Vec3 axis) {
    return glm::angleAxis(deg * kDeg2Rad, glm::normalize(axis));
}

} // namespace

TEST_CASE("compose_transform matches Transform::to_matrix exactly") {
    const Transform t = trs({ 1.5f, -2.0f, 3.25f }, axis_angle(37.0f, { 1, 2, 3 }), { 2.0f, 0.5f, -1.5f });
    CHECK(scene::compose_transform(t) == t.to_matrix());
}

TEST_CASE("parent translate/rotate/scale propagate to exact child world matrices") {
    World        w;
    const Entity p  = w.create("P");
    const Entity c  = w.create_child(p, "C");
    const Entity gc = w.create_child(c, "GC");
    const Transform tp = trs({ 10, 0, -5 }, axis_angle(90.0f, { 0, 1, 0 }), { 2, 2, 2 });
    const Transform tc = trs({ 1, 2, 3 }, axis_angle(45.0f, { 1, 0, 0 }), { 1, 3, 1 });
    const Transform tg = trs({ 0, 0, 1 }, Quat{ 1, 0, 0, 0 }, { 0.5f, 0.5f, 0.5f });
    scene::set_local_transform(w, p, tp);
    scene::set_local_transform(w, c, tc);
    scene::set_local_transform(w, gc, tg);
    w.update_transforms();

    const Mat4 ep = tp.to_matrix();
    const Mat4 ec = ep * tc.to_matrix();
    const Mat4 eg = ec * tg.to_matrix();
    CHECK(mat_near(w.get<TransformComponent>(p).world, ep));
    CHECK(mat_near(w.get<TransformComponent>(c).world, ec));
    CHECK(mat_near(w.get<TransformComponent>(gc).world, eg));
    CHECK(mat_near(w.world_matrix(gc), eg));
    // The child's origin: parent rotates +X onto -Z and scales by 2.
    CHECK(vec_near(scene::world_position(w, c), { 10 + 2 * 3, 4, -5 - 2 * 1 }));
    CHECK_FALSE(w.get<TransformComponent>(gc).dirty);

    // world_matrix() is correct while dirty (before update_transforms).
    scene::set_local_position(w, p, { 0, 100, 0 });
    const Transform tp2 = trs({ 0, 100, 0 }, tp.rotation, tp.scale);
    CHECK(mat_near(w.world_matrix(gc), tp2.to_matrix() * tc.to_matrix() * tg.to_matrix()));
    CHECK(mat_near(w.get<TransformComponent>(gc).world, eg)); // cache still stale
    w.update_transforms();
    CHECK(mat_near(w.get<TransformComponent>(gc).world, w.world_matrix(gc)));
    CHECK(mat_near(w.world_matrix(kNull), Mat4(1.0f)));
}

TEST_CASE("only dirty subtrees are recomputed") {
    World        w;
    const Entity a  = w.create("A");
    const Entity b  = w.create_child(a, "B");
    const Entity c  = w.create_child(a, "C");
    const Entity b1 = w.create_child(b, "B1");
    const Entity b2 = w.create_child(b, "B2");
    const Entity c1 = w.create_child(c, "C1");
    w.update_transforms();
    auto stats = scene::last_transform_update_stats(w);
    CHECK(stats.dirty_found == 6);
    CHECK(stats.dirty_roots == 1);
    CHECK(stats.recomputed == 6);

    w.update_transforms(); // nothing dirty
    stats = scene::last_transform_update_stats(w);
    CHECK(stats.dirty_found == 0);
    CHECK(stats.recomputed == 0);

    // Poison clean caches: if they were recomputed the sentinel would disappear.
    const Mat4 sentinel(42.0f);
    w.get<TransformComponent>(c).world  = sentinel;
    w.get<TransformComponent>(c1).world = sentinel;
    w.get<TransformComponent>(a).world  = Mat4(1.0f);

    scene::set_local_position(w, b, { 1, 0, 0 });
    w.update_transforms();
    stats = scene::last_transform_update_stats(w);
    CHECK(stats.dirty_found == 1);
    CHECK(stats.dirty_roots == 1);
    CHECK(stats.recomputed == 3); // b, b1, b2
    CHECK(w.get<TransformComponent>(c).world == sentinel);
    CHECK(w.get<TransformComponent>(c1).world == sentinel);
    CHECK(vec_near(Vec3(w.get<TransformComponent>(b2).world[3]), { 1, 0, 0 }));

    // A dirty ancestor covers dirty descendants: one root, whole tree once.
    scene::set_local_position(w, b1, { 0, 1, 0 });
    scene::set_local_position(w, c1, { 0, 0, 1 });
    scene::set_local_position(w, a, { 5, 0, 0 });
    w.update_transforms();
    stats = scene::last_transform_update_stats(w);
    CHECK(stats.dirty_found == 3);
    CHECK(stats.dirty_roots == 1);
    CHECK(stats.recomputed == 6);
    CHECK(vec_near(Vec3(w.get<TransformComponent>(b1).world[3]), { 6, 1, 0 }));
    CHECK(vec_near(Vec3(w.get<TransformComponent>(c1).world[3]), { 5, 0, 1 }));

    // Two disjoint dirty subtrees.
    scene::set_local_position(w, b1, { 0, 2, 0 });
    scene::set_local_position(w, c1, { 0, 0, 2 });
    w.update_transforms();
    stats = scene::last_transform_update_stats(w);
    CHECK(stats.dirty_roots == 2);
    CHECK(stats.recomputed == 2);

    // Reparenting keeps LOCAL and dirties the moved subtree only.
    w.set_parent(b, c);
    CHECK(vec_near(scene::local_transform(w, b).position, { 1, 0, 0 }));
    w.update_transforms();
    stats = scene::last_transform_update_stats(w);
    CHECK(stats.recomputed == 3);
    CHECK(vec_near(Vec3(w.get<TransformComponent>(b1).world[3]), { 6, 2, 0 }));
}

TEST_CASE("100k-entity deep chain and wide tree") {
    constexpr int kCount = 100000;

    SUBCASE("deep chain") {
        World  w;
        Entity root = w.create("0");
        scene::set_local_position(w, root, { 1, 0, 0 });
        Entity cur = root;
        for (int i = 1; i < kCount; ++i) {
            cur = w.create_child(cur);
            scene::set_local_position(w, cur, { 1, 0, 0 });
        }
        const Entity leaf = cur;
        CHECK(w.entity_count() == kCount);
        w.update_transforms();
        auto stats = scene::last_transform_update_stats(w);
        CHECK(stats.dirty_roots == 1);
        CHECK(stats.recomputed == kCount);
        CHECK(w.get<TransformComponent>(leaf).world[3].x == static_cast<f32>(kCount));
        CHECK(scene::depth_of(w, leaf) == kCount - 1);

        // Leaf-only dirty: resolving it walks the chain once.
        scene::set_local_position(w, leaf, { 2, 0, 0 });
        w.update_transforms();
        stats = scene::last_transform_update_stats(w);
        CHECK(stats.dirty_roots == 1);
        CHECK(stats.recomputed == 1);
        CHECK(w.get<TransformComponent>(leaf).world[3].x == static_cast<f32>(kCount + 1));

        // Root dirty: everything once.
        scene::set_local_position(w, root, { 0, 0, 0 });
        w.update_transforms();
        CHECK(scene::last_transform_update_stats(w).recomputed == kCount);
        CHECK(w.get<TransformComponent>(leaf).world[3].x == static_cast<f32>(kCount));

        // Every entity dirty (worst case for dirty-root resolution): still one root.
        for (auto [e, tc] : w.view<TransformComponent>().each()) {
            tc.dirty = true;
        }
        w.update_transforms();
        stats = scene::last_transform_update_stats(w);
        CHECK(stats.dirty_found == kCount);
        CHECK(stats.dirty_roots == 1);
        CHECK(stats.recomputed == kCount);

        // Steady state is allocation-free.
        scene::set_local_position(w, root, { 1, 0, 0 });
        scene::set_local_position(w, leaf, { 1, 0, 0 });
        const long long before = g_allocations.load();
        w.update_transforms();
        const long long allocs = g_allocations.load() - before;
        CHECK(allocs == 0);
        CHECK(w.get<TransformComponent>(leaf).world[3].x == static_cast<f32>(kCount));

        w.destroy(root); // iterative subtree destruction, no recursion
        CHECK(w.entity_count() == 0);
    }

    SUBCASE("wide tree") {
        World        w;
        const Entity root = w.create("Root");
        for (int i = 0; i < kCount; ++i) {
            const Entity c = w.create_child(root);
            scene::set_local_position(w, c, { static_cast<f32>(i), 0, 0 });
        }
        w.update_transforms();
        CHECK(scene::last_transform_update_stats(w).recomputed == kCount + 1);
        CHECK(scene::child_count_of(w, root) == kCount);

        scene::set_local_transform(w, root, trs({ 0, 10, 0 }, Quat{ 1, 0, 0, 0 }, { 2, 2, 2 }));
        const long long before = g_allocations.load();
        w.update_transforms();
        CHECK(g_allocations.load() - before == 0);
        CHECK(scene::last_transform_update_stats(w).recomputed == kCount + 1);

        bool all_ok = true;
        int  i      = 0;
        scene::for_each_child(w, root, [&](Entity c) {
            const Vec3 p(w.get<TransformComponent>(c).world[3]);
            all_ok = all_ok && p == Vec3(2.0f * static_cast<f32>(i), 10.0f, 0.0f);
            ++i;
        });
        CHECK(all_ok);
        CHECK(i == kCount);
        check_valid(w);
    }
}

TEST_CASE("set_parent_keep_world preserves the world transform") {
    World        w;
    const Entity p = w.create("P");
    const Entity q = w.create("Q");
    const Entity c = w.create_child(p, "C");
    scene::set_local_transform(w, p, trs({ 3, 4, 5 }, axis_angle(30.0f, { 0, 0, 1 }), { 2, 2, 2 }));
    scene::set_local_transform(w, q, trs({ -7, 1, 0 }, axis_angle(-75.0f, { 1, 1, 0 }), { 0.5f, 0.5f, 0.5f }));
    scene::set_local_transform(w, c, trs({ 1, 0, 0 }, axis_angle(10.0f, { 0, 1, 0 }), { 1, 1, 1 }));
    w.update_transforms();
    const Mat4 before = w.world_matrix(c);

    CHECK(scene::set_parent_keep_world(w, c, q));
    CHECK(scene::parent_of(w, c) == q);
    w.update_transforms();
    CHECK(mat_near(w.world_matrix(c), before));

    CHECK(scene::set_parent_keep_world(w, c, kNull));
    w.update_transforms();
    CHECK(mat_near(w.world_matrix(c), before));
    CHECK(mat_near(scene::local_transform(w, c).to_matrix(), before));

    // Rejected reparent changes nothing.
    const Entity cc = w.create_child(c, "CC");
    const Transform local = scene::local_transform(w, c);
    CHECK_FALSE(scene::set_parent_keep_world(w, c, cc));
    CHECK(scene::parent_of(w, c) == kNull);
    CHECK(scene::local_transform(w, c).position == local.position);

    // Non-uniform, axis-aligned parent scale is exact when the child is axis-aligned too
    // (a rotated child under a non-uniformly scaled parent needs shear — documented limit).
    scene::set_local_transform(w, c, trs({ 1, -2, 3 }, Quat{ 1, 0, 0, 0 }, { 3, 1, 2 }));
    scene::set_local_transform(w, q, trs({ 1, 2, 3 }, Quat{ 1, 0, 0, 0 }, { 1, 2, 4 }));
    w.update_transforms();
    const Mat4 before2 = w.world_matrix(c);
    CHECK(scene::set_parent_keep_world(w, c, q));
    w.update_transforms();
    CHECK(mat_near(w.world_matrix(c), before2));
}

TEST_CASE("set_world_position / set_world_matrix and decomposition") {
    World        w;
    const Entity p = w.create("P");
    const Entity c = w.create_child(p, "C");
    scene::set_local_transform(w, p, trs({ 5, 0, 0 }, axis_angle(90.0f, { 0, 0, 1 }), { 2, 2, 2 }));
    scene::set_world_position(w, c, { 1, 2, 3 });
    w.update_transforms();
    CHECK(vec_near(scene::world_position(w, c), { 1, 2, 3 }));

    const Mat4 target = trs({ -4, 8, 1 }, axis_angle(120.0f, { 1, -1, 2 }), { 3, 3, 3 }).to_matrix();
    scene::set_world_matrix(w, c, target);
    w.update_transforms();
    CHECK(mat_near(w.world_matrix(c), target));

    const Transform t = trs({ 1, 2, 3 }, axis_angle(200.0f, { 0.3f, 1, -0.2f }), { -2, 0.5f, 3 });
    const Transform d = scene::decompose_transform(t.to_matrix());
    CHECK(mat_near(d.to_matrix(), t.to_matrix()));
}
