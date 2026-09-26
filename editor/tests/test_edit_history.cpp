// test_edit_history.cpp — snapshot undo/redo against a real World and the scene serializer.
#include "aether/editor/edit_history.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::editor;

namespace {
EditHistory make_history(usize max_steps = 128) {
    return EditHistory(
        [](World& w, const std::string& snap) -> Result<void> {
            auto r = scene::load_scene_from_string(w, snap);
            if (!r) {
                return r.error();
            }
            return {};
        },
        max_steps);
}
f32 x_of(World& w, const char* name) { return scene::local_transform(w, scene::find_by_name(w, name)).position.x; }
} // namespace

TEST_CASE("discrete edits undo and redo") {
    World w;
    EditHistory h = make_history();
    const Entity a = w.create("A");
    h.mark_clean(w);
    CHECK_FALSE(h.can_undo());

    scene::set_local_position(w, a, Vec3(1, 0, 0));
    h.record("Move A", w);
    w.create("B");
    h.record("Create B", w);
    CHECK(h.undo_count() == 2);
    CHECK(h.next_undo_label() == "Create B");
    CHECK(h.dirty());

    CHECK(h.undo(w) == std::optional<std::string>("Create B"));
    CHECK((scene::find_by_name(w, "B") == kNullEntity));
    CHECK(x_of(w, "A") == 1.0f);
    CHECK(h.undo(w) == std::optional<std::string>("Move A"));
    CHECK(x_of(w, "A") == 0.0f);
    CHECK_FALSE(h.undo(w).has_value());
    CHECK(h.redo_count() == 2);

    CHECK(h.redo(w) == std::optional<std::string>("Move A"));
    CHECK(x_of(w, "A") == 1.0f);
    CHECK(h.redo(w) == std::optional<std::string>("Create B"));
    CHECK((scene::find_by_name(w, "B") != kNullEntity));
    CHECK_FALSE(h.redo(w).has_value());
}

TEST_CASE("a new edit clears the redo stack") {
    World w;
    EditHistory h = make_history();
    w.create("A");
    h.mark_clean(w);
    w.create("B");
    h.record("B", w);
    (void)h.undo(w);
    CHECK(h.can_redo());
    w.create("C");
    h.record("C", w);
    CHECK_FALSE(h.can_redo());
    (void)h.undo(w);
    CHECK((scene::find_by_name(w, "C") == kNullEntity));
    CHECK((scene::find_by_name(w, "B") == kNullEntity));
}

TEST_CASE("continuous edits collapse into one step") {
    World w;
    EditHistory h = make_history();
    const Entity a = w.create("A");
    h.mark_clean(w);
    for (int i = 1; i <= 10; ++i) {
        scene::set_local_position(w, a, Vec3(static_cast<f32>(i), 0, 0));
        h.begin_continuous("Drag");
    }
    CHECK(h.in_continuous());
    h.end_continuous(w);
    CHECK(h.undo_count() == 1);
    (void)h.undo(w);
    CHECK(x_of(w, "A") == 0.0f);
    (void)h.redo(w);
    CHECK(x_of(w, "A") == 10.0f);

    // Undo during an open gesture closes it first (the gesture's end state is redoable).
    scene::set_local_position(w, scene::find_by_name(w, "A"), Vec3(20, 0, 0));
    h.begin_continuous("Drag 2");
    CHECK(h.undo(w) == std::optional<std::string>("Drag 2"));
    CHECK(x_of(w, "A") == 10.0f);
    (void)h.redo(w);
    CHECK(x_of(w, "A") == 20.0f);
}

TEST_CASE("step limit, saved flag and clear") {
    World w;
    EditHistory h = make_history(3);
    h.mark_clean(w);
    for (int i = 0; i < 5; ++i) {
        w.create("E");
        h.record("Create", w);
    }
    CHECK(h.undo_count() == 3);
    h.mark_saved();
    CHECK_FALSE(h.dirty());
    (void)h.undo(w);
    CHECK(h.dirty());
    CHECK(w.entity_count() == 4);
    h.clear();
    CHECK_FALSE(h.can_undo());
    CHECK_FALSE(h.can_redo());
}

TEST_CASE("hierarchy and component edits are covered by snapshots") {
    World w;
    EditHistory h = make_history();
    const Entity parent = w.create("Parent");
    const Entity child  = w.create("Child");
    h.mark_clean(w);
    w.set_parent(child, parent);
    w.add<LightComponent>(child);
    h.record("Parent + light", w);
    (void)h.undo(w);
    const Entity c2 = scene::find_by_name(w, "Child");
    CHECK((scene::parent_of(w, c2) == kNullEntity));
    CHECK_FALSE(w.has<LightComponent>(c2));
    (void)h.redo(w);
    const Entity c3 = scene::find_by_name(w, "Child");
    CHECK((scene::parent_of(w, c3) == scene::find_by_name(w, "Parent")));
    CHECK(w.has<LightComponent>(c3));
}
