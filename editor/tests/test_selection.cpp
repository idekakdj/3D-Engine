// test_selection.cpp — multi-selection set semantics and selection roots.
#include "aether/editor/multi_edit.h"
#include "aether/editor/selection.h"
#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

#include <vector>

using namespace aether;
using namespace aether::editor;

TEST_CASE("Selection: click modes") {
    SelectionSet s;
    s.apply_click(10, SelectMode::Replace);
    CHECK(s.size() == 1);
    CHECK(s.primary() == 10);

    s.apply_click(20, SelectMode::Add); // shift
    s.apply_click(30, SelectMode::Toggle); // ctrl adds when absent
    CHECK(s.uuids() == std::vector<u64>{ 10, 20, 30 });
    CHECK(s.primary() == 30);

    s.apply_click(20, SelectMode::Toggle); // ctrl removes when present
    CHECK(s.uuids() == std::vector<u64>{ 10, 30 });
    CHECK_FALSE(s.contains(20));

    s.apply_click(10, SelectMode::Add); // re-adding moves to primary
    CHECK(s.uuids() == std::vector<u64>{ 30, 10 });
    CHECK(s.primary() == 10);

    s.apply_click(0, SelectMode::Toggle); // empty space with modifiers: unchanged
    s.apply_click(0, SelectMode::Add);
    CHECK(s.size() == 2);
    s.apply_click(0, SelectMode::Replace); // plain click on nothing clears
    CHECK(s.empty());
    CHECK(s.primary() == 0);
}

TEST_CASE("Selection: set_all drops zeros and duplicates, last is primary") {
    SelectionSet    s;
    const u64 in[] = { 5, 0, 7, 5, 9 };
    s.set_all(in);
    CHECK(s.uuids() == std::vector<u64>{ 7, 5, 9 });
    CHECK(s.primary() == 9);
    CHECK(s.remove(5));
    CHECK_FALSE(s.remove(5));
    CHECK(s.uuids() == std::vector<u64>{ 7, 9 });
}

TEST_CASE("Selection: modifier mapping") {
    CHECK(select_mode_from(false, false) == SelectMode::Replace);
    CHECK(select_mode_from(true, false) == SelectMode::Toggle);
    CHECK(select_mode_from(false, true) == SelectMode::Add);
    CHECK(select_mode_from(true, true) == SelectMode::Toggle);
}

TEST_CASE("Selection: prune, entities and roots against a world") {
    World        w;
    const Entity a  = w.create("A");
    const Entity a1 = w.create_child(a, "A1");
    const Entity a2 = w.create_child(a1, "A2");
    const Entity b  = w.create("B");

    SelectionSet s;
    for (const Entity e : { a2, a, b }) {
        s.add(scene::uuid_of(w, e));
    }
    CHECK(s.entities(w) == std::vector<Entity>{ a2, a, b });

    // A2 has a selected ancestor (A): only A and B are roots, in selection order.
    const std::vector<Entity> sel = s.entities(w);
    CHECK(selection_roots(w, sel) == std::vector<Entity>{ a, b });
    const Entity only_child[] = { a1, a2 };
    CHECK(selection_roots(w, only_child) == std::vector<Entity>{ a1 });
    const Entity dup[] = { b, b, kNullEntity };
    CHECK(selection_roots(w, dup) == std::vector<Entity>{ b });

    w.destroy(a); // takes A1 / A2 with it
    CHECK(s.prune(w) == 2);
    CHECK(s.uuids() == std::vector<u64>{ scene::uuid_of(w, b) });
    CHECK(s.primary() == scene::uuid_of(w, b));
}

TEST_CASE("Multi-edit: only the edited fields propagate") {
    World        w;
    const Entity a = w.create("A");
    const Entity b = w.create("B");
    const Entity c = w.create("C"); // no light: untouched
    LightComponent la;
    la.intensity = 1.0f;
    la.color     = Vec3(1, 0, 0);
    LightComponent lb;
    lb.intensity = 5.0f;
    lb.color     = Vec3(0, 0, 1);
    w.add<LightComponent>(a, la);
    w.add<LightComponent>(b, lb);

    const LightComponent before = w.get<LightComponent>(a);
    w.get<LightComponent>(a).intensity = 3.0f; // the inspector edited the primary's intensity
    const Entity others[] = { b, c };
    CHECK(propagate_fields(w, before, w.get<LightComponent>(a), others, &LightComponent::intensity, &LightComponent::color));
    CHECK(w.get<LightComponent>(b).intensity == 3.0f);
    CHECK(w.get<LightComponent>(b).color == Vec3(0, 0, 1)); // not edited: kept
    CHECK_FALSE(w.has<LightComponent>(c));
    CHECK_FALSE(propagate_fields(w, w.get<LightComponent>(a), w.get<LightComponent>(a), others, &LightComponent::intensity));
}
