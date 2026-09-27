// test_prefab.cpp — prefab documents: pivot-relative roots, instantiation with placement, links,
// file round trip, apply / revert and the editor codecs inside scene files.
#include "aether/editor/prefab.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

using namespace aether;
using namespace aether::editor;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps))); }

struct Fixture {
    World  w;
    Entity a, a_child, b;
    Fixture() {
        REQUIRE(register_editor_codecs(w));
        a       = w.create("A");
        a_child = w.create_child(a, "AChild");
        b       = w.create("B");
        scene::set_local_position(w, a, Vec3(2, 0, 0));
        scene::set_local_position(w, a_child, Vec3(0, 1, 0));
        scene::set_local_position(w, b, Vec3(4, 0, 0));
        scene::set_local_scale(w, b, Vec3(2.0f));
        w.add<PrefabInstanceComponent>(a, PrefabInstanceComponent{ "prefabs/old.aeprefab" });       // top-level link: stripped
        w.add<PrefabInstanceComponent>(a_child, PrefabInstanceComponent{ "prefabs/nested.aeprefab" }); // nested: kept
        w.update_transforms();
    }
};
} // namespace

TEST_CASE("Prefab: document is pivot-relative and instantiates with placement and links") {
    Fixture      f;
    const Entity roots[] = { f.a, f.b, f.a_child }; // the child folds into A
    const Mat4   pivot   = glm::translate(Mat4(1.0f), Vec3(3, 0, 0));
    auto         doc     = make_prefab_document(f.w, roots, pivot);
    REQUIRE(doc.has_value());
    CHECK(doc->find("prefabs/old.aeprefab") == std::string::npos);
    CHECK(doc->find("prefabs/nested.aeprefab") != std::string::npos);

    World dst;
    REQUIRE(register_editor_codecs(dst));
    auto inst = instantiate_prefab_document(dst, *doc, "prefabs/pair.aeprefab", kNullEntity,
                                            glm::translate(Mat4(1.0f), Vec3(10, 0, 0)));
    REQUIRE(inst.has_value());
    REQUIRE(inst->size() == 2);
    const Entity ia = (*inst)[0];
    const Entity ib = (*inst)[1];
    CHECK(dst.get<NameComponent>(ia).name == "A");
    CHECK(near(scene::world_position(dst, ia), Vec3(9, 0, 0)));  // 10 + (2 - 3)
    CHECK(near(scene::world_position(dst, ib), Vec3(11, 0, 0))); // 10 + (4 - 3)
    CHECK(near(scene::local_transform(dst, ib).scale, Vec3(2.0f)));
    CHECK(dst.get<PrefabInstanceComponent>(ia).source == "prefabs/pair.aeprefab");
    CHECK(dst.get<PrefabInstanceComponent>(ib).source == "prefabs/pair.aeprefab");
    const Entity child = scene::find_child_by_name(dst, ia, "AChild");
    REQUIRE((child != kNullEntity));
    CHECK(near(scene::world_position(dst, child), Vec3(9, 1, 0)));
    CHECK(dst.get<PrefabInstanceComponent>(child).source == "prefabs/nested.aeprefab");
    CHECK(scene::uuid_of(dst, ia) != scene::uuid_of(f.w, f.a)); // fresh uuids

    // Instantiating as a child keeps the stored (pivot-relative) local transforms.
    const Entity parent = dst.create("Parent");
    auto         under  = instantiate_prefab_document(dst, *doc, "p", parent, Mat4(1.0f));
    REQUIRE(under.has_value());
    CHECK(scene::parent_of(dst, under->front()) == parent);
    CHECK(near(scene::local_transform(dst, under->front()).position, Vec3(-1, 0, 0)));
}

TEST_CASE("Prefab: file round trip, apply and revert") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_editor_prefab_test";
    std::filesystem::remove_all(dir);
    const std::filesystem::path file = dir / "sub" / "thing.aeprefab";

    Fixture      f;
    const Entity roots[] = { f.a };
    REQUIRE(write_prefab(f.w, roots, f.w.world_matrix(f.a), file).has_value());
    CHECK(std::filesystem::exists(file));
    CHECK(!instantiate_prefab(f.w, dir / "missing.aeprefab", "x", kNullEntity, Mat4(1.0f)).has_value());

    auto inst = instantiate_prefab(f.w, file, "thing.aeprefab", kNullEntity, glm::translate(Mat4(1.0f), Vec3(0, 0, 5)));
    REQUIRE(inst.has_value());
    REQUIRE(inst->size() == 1);
    Entity e = inst->front();
    CHECK(near(scene::world_position(f.w, e), Vec3(0, 0, 5)));

    // Local edits, then revert: the subtree comes back, the instance's placement and name stay.
    f.w.get<NameComponent>(e).name = "Renamed";
    const Entity child             = scene::find_child_by_name(f.w, e, "AChild");
    REQUIRE((child != kNullEntity));
    f.w.destroy(child);
    const Entity sibling_after = f.w.create("After");
    REQUIRE(scene::move_after(f.w, sibling_after, e));
    auto reverted = revert_prefab_instance(f.w, e, file);
    REQUIRE(reverted.has_value());
    e = *reverted;
    CHECK(f.w.get<NameComponent>(e).name == "Renamed");
    CHECK((scene::find_child_by_name(f.w, e, "AChild") != kNullEntity));
    CHECK(near(scene::world_position(f.w, e), Vec3(0, 0, 5)));
    CHECK(f.w.get<PrefabInstanceComponent>(e).source == "thing.aeprefab");
    CHECK(scene::next_sibling_of(f.w, e) == sibling_after); // kept its place among siblings

    // Apply: the instance's subtree becomes the prefab.
    f.w.create_child(e, "Added");
    REQUIRE(apply_prefab_instance(f.w, e, file).has_value());
    auto again = instantiate_prefab(f.w, file, "thing.aeprefab", kNullEntity, Mat4(1.0f));
    REQUIRE(again.has_value());
    CHECK((scene::find_child_by_name(f.w, again->front(), "Added") != kNullEntity));
    CHECK(near(scene::world_position(f.w, again->front()), Vec3(0.0f))); // root is the prefab origin
    std::filesystem::remove_all(dir);
}

TEST_CASE("Prefab: link component round-trips through scene files") {
    Fixture f;
    auto    text = scene::save_scene_to_string(f.w);
    REQUIRE(text.has_value());
    CHECK(text->find(std::string(kPrefabCodecName)) != std::string::npos);

    World dst;
    REQUIRE(register_editor_codecs(dst));
    auto loaded = scene::load_scene_from_string(dst, *text);
    REQUIRE(loaded.has_value());
    CHECK(loaded->warning_count == 0);
    const Entity a = scene::find_by_name(dst, "A");
    REQUIRE((a != kNullEntity));
    CHECK(dst.get<PrefabInstanceComponent>(a).source == "prefabs/old.aeprefab");

    // A malformed link is skipped with a warning, not a failure.
    World bad;
    REQUIRE(register_editor_codecs(bad));
    std::string broken = *text;
    const auto  pos    = broken.find("\"source\"");
    REQUIRE(pos != std::string::npos);
    broken.replace(pos, 8, "\"nosrc\"");
    auto r = scene::load_scene_from_string(bad, broken);
    REQUIRE(r.has_value());
    CHECK(r->warning_count >= 1);
}

TEST_CASE("Prefab: multi-root instances revert to their own root; apply is refused") {
    const std::filesystem::path dir  = std::filesystem::temp_directory_path() / "aether_editor_prefab_multi";
    const std::filesystem::path file = dir / "pair.aeprefab";
    std::filesystem::remove_all(dir);
    Fixture      f;
    const Entity roots[] = { f.a, f.b };
    REQUIRE(write_prefab(f.w, roots, Mat4(1.0f), file).has_value());
    auto text = read_text_file(file);
    REQUIRE(text.has_value());
    CHECK(prefab_root_count(*text) == 2);
    CHECK(prefab_root_count("{}") == 0);

    auto inst = instantiate_prefab(f.w, file, "pair", kNullEntity, Mat4(1.0f));
    REQUIRE(inst.has_value());
    REQUIRE(inst->size() == 2);
    CHECK(f.w.get<PrefabInstanceComponent>((*inst)[1]).root_index == 1);
    f.w.get<NameComponent>((*inst)[1]).name = "B instance";
    auto reverted = revert_prefab_instance(f.w, (*inst)[1], file);
    REQUIRE(reverted.has_value());
    CHECK(f.w.get<NameComponent>(*reverted).name == "B instance"); // name kept
    CHECK((scene::find_child_by_name(f.w, *reverted, "AChild") == kNullEntity)); // it is B, not A
    CHECK(f.w.get<PrefabInstanceComponent>(*reverted).root_index == 1);
    CHECK(near(scene::local_transform(f.w, *reverted).scale, Vec3(2.0f))); // B's own scale
    CHECK_FALSE(apply_prefab_instance(f.w, *reverted, file).has_value());
    std::filesystem::remove_all(dir);
}
