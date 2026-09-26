// test_world.cpp — World lifecycle, hierarchy maintenance, uuids, visibility, hierarchy utils.
#include "scene_test_utils.h"

#include <set>

using namespace aether;
using namespace scene_test;

TEST_CASE("create adds the standard component set; counts track create/destroy") {
    World w;
    CHECK(w.entity_count() == 0);
    const Entity a = w.create("A");
    CHECK(w.valid(a));
    CHECK(w.has<NameComponent>(a));
    CHECK(w.has<TransformComponent>(a));
    CHECK(w.has<HierarchyComponent>(a));
    CHECK(w.has<VisibilityComponent>(a));
    CHECK(w.has<IdComponent>(a));
    CHECK(w.get<NameComponent>(a).name == "A");
    CHECK(w.get<TransformComponent>(a).dirty);
    CHECK(scene::uuid_of(w, a) != scene::kInvalidUuid);

    const Entity b = w.create();
    const Entity c = w.create_child(a, "C");
    CHECK(w.entity_count() == 3);
    CHECK(w.get<NameComponent>(b).name == "Entity");
    CHECK(scene::parent_of(w, c) == a);

    w.destroy(b);
    CHECK_FALSE(w.valid(b));
    CHECK(w.entity_count() == 2);
    w.destroy(b);           // double destroy: no-op
    w.destroy(kNull); // null: no-op
    CHECK(w.entity_count() == 2);
    check_valid(w);

    w.clear();
    CHECK(w.entity_count() == 0);
    CHECK(scene::root_count(w) == 0);
    CHECK(scene::first_root(w) == kNull);
    CHECK(scene::find_by_uuid(w, 1) == kNull);
    const Entity d = w.create("D"); // world fully usable after clear
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ d });
    check_valid(w);
}

TEST_CASE("destroy is recursive and keeps the parent's list consistent") {
    World        w;
    const Entity root = w.create("Root");
    const Entity a    = w.create_child(root, "A");
    const Entity b    = w.create_child(root, "B");
    const Entity c    = w.create_child(root, "C");
    const Entity b1   = w.create_child(b, "B1");
    const Entity b2   = w.create_child(b, "B2");
    const Entity b11  = w.create_child(b1, "B11");
    CHECK(w.entity_count() == 7);

    w.destroy(b); // b, b1, b2, b11
    CHECK(w.entity_count() == 3);
    for (Entity gone : { b, b1, b2, b11 }) {
        CHECK_FALSE(w.valid(gone));
    }
    CHECK(checked_children(w, root) == std::vector<Entity>{ a, c });
    check_valid(w);

    w.destroy(root);
    CHECK(w.entity_count() == 0);
    CHECK(scene::root_count(w) == 0);
    check_valid(w);
}

TEST_CASE("create_child with an invalid parent creates a root") {
    World        w;
    const Entity dead = w.create("Dead");
    w.destroy(dead);
    const Entity e = w.create_child(dead, "Orphan");
    CHECK(w.valid(e));
    CHECK(scene::parent_of(w, e) == kNull);
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ e });
}

TEST_CASE("reparent and detach keep sibling lists consistent in both directions") {
    World        w;
    const Entity p = w.create("P");
    const Entity q = w.create("Q");
    const Entity a = w.create_child(p, "A");
    const Entity b = w.create_child(p, "B");
    const Entity c = w.create_child(p, "C");
    CHECK(checked_children(w, p) == std::vector<Entity>{ a, b, c });
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ p, q });

    // Middle child to another parent.
    w.set_parent(b, q);
    CHECK(checked_children(w, p) == std::vector<Entity>{ a, c });
    CHECK(checked_children(w, q) == std::vector<Entity>{ b });
    // Head child.
    w.set_parent(a, q);
    CHECK(checked_children(w, p) == std::vector<Entity>{ c });
    CHECK(checked_children(w, q) == std::vector<Entity>{ b, a });
    // Tail/only child detached -> becomes last root.
    w.set_parent(c, kNull);
    CHECK(checked_children(w, p).empty());
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ p, q, c });
    // Root reattached (removed from the middle of the root list).
    w.set_parent(q, c);
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ p, c });
    CHECK(checked_children(w, c) == std::vector<Entity>{ q });
    // Same parent again: no-op, order preserved.
    w.set_parent(b, q);
    CHECK(checked_children(w, q) == std::vector<Entity>{ b, a });
    check_valid(w);

    // Reordering.
    CHECK(scene::move_before(w, a, b));
    CHECK(checked_children(w, q) == std::vector<Entity>{ a, b });
    CHECK(scene::move_after(w, a, b));
    CHECK(checked_children(w, q) == std::vector<Entity>{ b, a });
    CHECK(scene::move_before(w, p, c)); // roots reorder too
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ p, c });
    CHECK(scene::move_after(w, p, c));
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ c, p });
    // move_before across parents reparents.
    CHECK(scene::move_before(w, p, b));
    CHECK(checked_children(w, q) == std::vector<Entity>{ p, b, a });
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ c });
    check_valid(w);
}

TEST_CASE("cycles and self-parenting are rejected without side effects") {
    World        w;
    const Entity a  = w.create("A");
    const Entity b  = w.create_child(a, "B");
    const Entity c  = w.create_child(b, "C");
    const Entity d  = w.create("D");
    w.update_transforms();

    w.set_parent(a, a);
    w.set_parent(a, c); // c is a descendant of a
    w.set_parent(b, c);
    CHECK(scene::parent_of(w, a) == kNull);
    CHECK(scene::parent_of(w, b) == a);
    CHECK(scene::parent_of(w, c) == b);
    CHECK_FALSE(w.get<TransformComponent>(a).dirty); // rejected => untouched
    CHECK_FALSE(scene::can_set_parent(w, a, c));
    CHECK(scene::can_set_parent(w, c, d));
    CHECK_FALSE(scene::move_before(w, a, c));
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ a, d });

    const Entity dead = w.create("Dead");
    w.destroy(dead);
    w.set_parent(a, dead); // invalid parent
    w.set_parent(dead, a); // invalid child
    CHECK(scene::parent_of(w, a) == kNull);
    check_valid(w);
}

TEST_CASE("raw registry destroy / component removal orphans children safely") {
    World        w;
    const Entity p  = w.create("P");
    const Entity a  = w.create_child(p, "A");
    const Entity b  = w.create_child(p, "B");
    const Entity a1 = w.create_child(a, "A1");
    w.registry().destroy(p); // bypasses World::destroy
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ a, b });
    CHECK(checked_children(w, a) == std::vector<Entity>{ a1 });
    check_valid(w);

    w.remove<HierarchyComponent>(a); // leaves the hierarchy; a1 becomes a root
    CHECK(checked_children(w, kNull) == std::vector<Entity>{ b, a1 });
    check_valid(w);

    // Adding a HierarchyComponent (with a requested parent) links it.
    w.add<HierarchyComponent>(a, HierarchyComponent{ b });
    CHECK(checked_children(w, b) == std::vector<Entity>{ a });
    check_valid(w);
}

TEST_CASE("uuids are unique, looked up in O(1) and re-keyable") {
    World             w;
    std::set<u64>     seen;
    std::vector<Entity> es;
    for (int i = 0; i < 1000; ++i) {
        es.push_back(w.create());
        seen.insert(scene::uuid_of(w, es.back()));
    }
    CHECK(seen.size() == 1000);
    CHECK_FALSE(seen.contains(scene::kInvalidUuid));
    for (Entity e : es) {
        CHECK(scene::find_by_uuid(w, scene::uuid_of(w, e)) == e);
    }

    const u64 old = scene::uuid_of(w, es[0]);
    CHECK(scene::set_uuid(w, es[0], 0x1234));
    CHECK(scene::find_by_uuid(w, 0x1234) == es[0]);
    CHECK(scene::find_by_uuid(w, old) == kNull);
    CHECK_FALSE(scene::set_uuid(w, es[1], 0x1234)); // taken
    CHECK_FALSE(scene::set_uuid(w, es[1], 0));

    // Colliding emplace is re-keyed.
    const Entity raw = w.registry().create();
    w.add<IdComponent>(raw, IdComponent{ 0x1234 });
    CHECK(scene::uuid_of(w, raw) != 0x1234);
    CHECK(scene::find_by_uuid(w, 0x1234) == es[0]);
    CHECK(scene::find_by_uuid(w, scene::uuid_of(w, raw)) == raw);

    const u64 gone = scene::uuid_of(w, es[5]);
    w.destroy(es[5]);
    CHECK(scene::find_by_uuid(w, gone) == kNull);

    u64 parsed = 0;
    CHECK(scene::uuid_to_string(0xABCDEF0123456789ull) == "abcdef0123456789");
    CHECK(scene::uuid_from_string("ABCDEF0123456789", parsed));
    CHECK(parsed == 0xABCDEF0123456789ull);
    CHECK_FALSE(scene::uuid_from_string("xyz", parsed));
    CHECK_FALSE(scene::uuid_from_string("00000000000000000", parsed)); // 17 digits
}

TEST_CASE("visible_in_hierarchy is derived and propagated") {
    World        w;
    const Entity a  = w.create("A");
    const Entity b  = w.create_child(a, "B");
    const Entity c  = w.create_child(b, "C");
    const Entity d  = w.create("D");

    scene::set_visible(w, a, false);
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, a));
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, b));
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, c));
    CHECK(scene::is_visible(w, b));

    const Entity e = w.create_child(c, "E"); // created under a hidden branch
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, e));

    w.set_parent(b, d); // moved to a visible branch
    CHECK(scene::is_visible_in_hierarchy(w, b));
    CHECK(scene::is_visible_in_hierarchy(w, e));

    scene::set_visible(w, c, false);
    CHECK(scene::is_visible_in_hierarchy(w, b));
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, e));

    // Direct writes + full recompute.
    w.get<VisibilityComponent>(d).visible = false;
    scene::update_visibility(w);
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, b));
    // Late-added component on an entity with children.
    w.remove<VisibilityComponent>(b);
    w.get<VisibilityComponent>(d).visible = true;
    scene::update_visibility(w);
    w.add<VisibilityComponent>(b, VisibilityComponent{ false, true });
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, b));
    scene::set_visible(w, c, true);
    CHECK_FALSE(scene::is_visible_in_hierarchy(w, e));
    scene::set_visible(w, b, true);
    CHECK(scene::is_visible_in_hierarchy(w, e));
}

TEST_CASE("hierarchy utilities: traversal order, names, paths, ancestry") {
    World        w;
    const Entity root  = w.create("Root");
    const Entity arm   = w.create_child(root, "Arm");
    const Entity hand  = w.create_child(arm, "Hand");
    const Entity leg   = w.create_child(root, "Leg");
    const Entity arm2  = w.create_child(root, "Arm"); // duplicate name
    const Entity hand2 = w.create_child(arm2, "Finger");
    const Entity other = w.create("Other");

    std::vector<Entity> pre;
    scene::for_each_in_hierarchy(w, [&](Entity e) { pre.push_back(e); });
    CHECK(pre == std::vector<Entity>{ root, arm, hand, leg, arm2, hand2, other });

    std::vector<Entity> desc;
    scene::for_each_descendant(w, root, [&](Entity e) {
        desc.push_back(e);
        return e == arm ? scene::Visit::SkipChildren : scene::Visit::Continue;
    });
    CHECK(desc == std::vector<Entity>{ arm, leg, arm2, hand2 });

    std::vector<Entity> stopped;
    scene::for_each_descendant(w, root, [&](Entity e) {
        stopped.push_back(e);
        return e == hand ? scene::Visit::Stop : scene::Visit::Continue;
    });
    CHECK(stopped == std::vector<Entity>{ arm, hand });

    std::vector<Entity> roots;
    scene::for_each_root(w, [&](Entity e) { roots.push_back(e); });
    CHECK(roots == std::vector<Entity>{ root, other });

    // for_each_child tolerates destroying the visited child.
    World        w2;
    const Entity p2 = w2.create("P");
    for (int i = 0; i < 4; ++i) {
        (void)w2.create_child(p2);
    }
    scene::for_each_child(w2, p2, [&](Entity e) { w2.destroy(e); });
    CHECK(scene::child_count_of(w2, p2) == 0);
    check_valid(w2);

    CHECK(scene::find_by_name(w, "Hand") == hand);
    CHECK(scene::find_by_name(w, "Arm") == arm);
    CHECK(scene::find_by_name(w, "Nope") == kNull);
    CHECK(scene::find_child_by_name(w, root, "Leg") == leg);
    CHECK(scene::find_child_by_name(w, kNull, "Other") == other);
    CHECK(scene::find_descendant_by_name(w, root, "Finger") == hand2);

    CHECK(scene::find_by_path(w, "Root/Arm/Hand") == hand);
    CHECK(scene::find_by_path(w, "/Root/Arm/Hand") == hand);
    CHECK(scene::find_by_path(w, "Root/Arm/Finger") == hand2); // backtracks to 2nd "Arm"
    CHECK(scene::find_by_path(w, "Root/Arm/Toe") == kNull);
    CHECK(scene::find_by_path(w, "Other") == other);
    CHECK(scene::find_by_path(w, root, "Leg") == leg);
    CHECK(scene::find_by_path(w, root, "") == root);
    CHECK(scene::path_of(w, hand) == "Root/Arm/Hand");
    CHECK(scene::find_by_path(w, scene::path_of(w, hand2)) == hand2);

    CHECK(scene::is_ancestor(w, root, hand));
    CHECK(scene::is_ancestor(w, arm, hand));
    CHECK_FALSE(scene::is_ancestor(w, hand, arm));
    CHECK_FALSE(scene::is_ancestor(w, hand, hand));
    CHECK_FALSE(scene::is_ancestor(w, leg, hand));
    CHECK(scene::root_of(w, hand) == root);
    CHECK(scene::depth_of(w, hand) == 2);
    CHECK(scene::depth_of(w, root) == 0);
}
