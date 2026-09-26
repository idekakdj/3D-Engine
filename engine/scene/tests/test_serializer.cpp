// test_serializer.cpp — .aescene round trips, error handling, subtree copy/paste, cloning.
#include "scene_test_utils.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <format>
#include <set>
#include <string>

using namespace aether;
using namespace scene_test;

namespace {

// A component owned by "another module" — clone_entity must copy it generically.
struct ForeignComponent {
    int  value = 0;
    bool flag  = false;
};

Quat axis_angle(f32 deg, Vec3 axis) {
    return glm::angleAxis(deg * kDeg2Rad, glm::normalize(axis));
}

// Builds a world exercising every serialized component and a non-trivial hierarchy.
struct Fixture {
    World  w;
    Entity sun, player, body, head, cam, lamp, prop, raw_child;

    Fixture() {
        sun = w.create("Sun");
        w.add<LightComponent>(sun, LightComponent{ LightKind::Directional, { 1.0f, 0.95f, 0.8f },
                                                   3.5f, 0.0f, 0.0f, 0.0f, true });
        player = w.create("Player \"One\" \xC3\xA9"); // quotes + UTF-8
        scene::set_local_transform(w, player, Transform{ { 0.1f, -2.5f, 1e-7f },
                                                         axis_angle(33.3f, { 1, 2, 3 }),
                                                         { 1.0f, 2.0f, 0.3f } });
        w.add<TagComponent>(player, TagComponent{ 0xDEADBEEFu });
        body = w.create_child(player, "Body");
        MeshRendererComponent mr;
        mr.mesh         = AssetId{ 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
        mr.material     = AssetId{ 1, 2 };
        mr.cast_shadows = false;
        mr.local_bounds = AABB{ { -1.5f, 0.0f, -0.25f }, { 1.5f, 2.0f, 0.25f } };
        w.add<MeshRendererComponent>(body, mr);
        head = w.create_child(body, "Head");
        scene::set_visible(w, head, false);
        cam = w.create_child(player, "Camera");
        w.add<CameraComponent>(cam, CameraComponent{ 75.0f, 0.05f, 5000.0f, false });
        lamp = w.create_child(player, "Lamp");
        w.add<LightComponent>(lamp, LightComponent{ LightKind::Spot, { 0.2f, 0.4f, 1.0f }, 800.0f,
                                                    25.0f, 12.5f, 40.0f, false });
        prop = w.create("Prop");
        scene::set_local_scale(w, prop, { -1.0f, 1.0f, 1.0f });
        raw_child = w.create_child(prop, "Raw");
        w.update_transforms();
    }
};

// Structural + component equality between two worlds, matched by uuid.
void check_equal(const World& a, const World& b) {
    std::vector<Entity> ea, eb;
    scene::for_each_in_hierarchy(a, [&](Entity e) { ea.push_back(e); });
    scene::for_each_in_hierarchy(b, [&](Entity e) { eb.push_back(e); });
    REQUIRE(ea.size() == eb.size());
    CHECK(a.entity_count() == b.entity_count());
    for (usize i = 0; i < ea.size(); ++i) {
        const Entity x = ea[i], y = eb[i];
        CAPTURE(i);
        CHECK(scene::uuid_of(a, x) == scene::uuid_of(b, y)); // same order + same identity
        CHECK(scene::uuid_of(a, scene::parent_of(a, x)) == scene::uuid_of(b, scene::parent_of(b, y)));
        CHECK(scene::child_count_of(a, x) == scene::child_count_of(b, y));
        CHECK(a.try_get<NameComponent>(x)->name == b.try_get<NameComponent>(y)->name);

        const Transform& la = a.try_get<TransformComponent>(x)->local;
        const Transform& lb = b.try_get<TransformComponent>(y)->local;
        CHECK(la.position == lb.position); // bit-exact
        CHECK(la.rotation == lb.rotation);
        CHECK(la.scale == lb.scale);
        CHECK(mat_near(a.world_matrix(x), b.world_matrix(y), 0.0f));

        CHECK(a.try_get<VisibilityComponent>(x)->visible == b.try_get<VisibilityComponent>(y)->visible);
        CHECK(scene::is_visible_in_hierarchy(a, x) == scene::is_visible_in_hierarchy(b, y));

        CHECK((a.try_get<TagComponent>(x) == nullptr) == (b.try_get<TagComponent>(y) == nullptr));
        if (const auto* t = a.try_get<TagComponent>(x)) {
            CHECK(t->tag == b.try_get<TagComponent>(y)->tag);
        }
        CHECK((a.try_get<MeshRendererComponent>(x) == nullptr) ==
              (b.try_get<MeshRendererComponent>(y) == nullptr));
        if (const auto* m = a.try_get<MeshRendererComponent>(x)) {
            const auto* n = b.try_get<MeshRendererComponent>(y);
            CHECK(m->mesh == n->mesh);
            CHECK(m->material == n->material);
            CHECK(m->cast_shadows == n->cast_shadows);
            CHECK(m->local_bounds.min == n->local_bounds.min);
            CHECK(m->local_bounds.max == n->local_bounds.max);
        }
        CHECK((a.try_get<LightComponent>(x) == nullptr) == (b.try_get<LightComponent>(y) == nullptr));
        if (const auto* l = a.try_get<LightComponent>(x)) {
            const auto* m = b.try_get<LightComponent>(y);
            CHECK(l->kind == m->kind);
            CHECK(l->color == m->color);
            CHECK(l->intensity == m->intensity);
            CHECK(l->range == m->range);
            CHECK(l->inner_cone_deg == m->inner_cone_deg);
            CHECK(l->outer_cone_deg == m->outer_cone_deg);
            CHECK(l->cast_shadows == m->cast_shadows);
        }
        CHECK((a.try_get<CameraComponent>(x) == nullptr) == (b.try_get<CameraComponent>(y) == nullptr));
        if (const auto* c = a.try_get<CameraComponent>(x)) {
            const auto* d = b.try_get<CameraComponent>(y);
            CHECK(c->fov_y_deg == d->fov_y_deg);
            CHECK(c->near_z == d->near_z);
            CHECK(c->far_z == d->far_z);
            CHECK(c->primary == d->primary);
        }
    }
    check_valid(b);
}

std::filesystem::path temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / "aether_scene_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("scene round trip preserves every component, hierarchy order and uuids") {
    Fixture f;
    auto    text = scene::save_scene_to_string(f.w);
    REQUIRE(text);
    CHECK(text->find("\"format\": \"aether.scene\"") != std::string::npos);
    CHECK(text->find("0123456789abcdeffedcba9876543210") != std::string::npos); // AssetId hex

    World loaded;
    (void)loaded.create("Existing"); // clear_world replaces it
    auto result = scene::load_scene_from_string(loaded, *text);
    REQUIRE(result);
    CHECK(result->entity_count == 8);
    CHECK(result->warning_count == 0);
    CHECK(result->roots.size() == 3);
    check_equal(f.w, loaded);

    // find_by_uuid works on the loaded world.
    const Entity head = scene::find_by_uuid(loaded, scene::uuid_of(f.w, f.head));
    REQUIRE(head != kNull);
    CHECK(loaded.get<NameComponent>(head).name == "Head");
    CHECK(scene::path_of(loaded, head) == scene::path_of(f.w, f.head));

    // Idempotent: saving the loaded world reproduces the same document.
    auto again = scene::save_scene_to_string(loaded);
    REQUIRE(again);
    CHECK(*again == *text);
}

TEST_CASE("file save/load round trip (atomic write, PIE-style snapshot)") {
    Fixture   f;
    const auto file = temp_dir() / "roundtrip.aescene";
    REQUIRE(scene::save_scene(f.w, file));
    CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));

    // Snapshot -> mutate -> restore, like play-in-editor.
    scene::set_local_position(f.w, f.player, { 99, 99, 99 });
    f.w.destroy(f.body);
    (void)f.w.create("Spawned");
    auto result = scene::load_scene(f.w, file);
    REQUIRE(result);

    World reference;
    REQUIRE(scene::load_scene(reference, file));
    check_equal(reference, f.w);
    CHECK(scene::find_by_name(f.w, "Spawned") == kNull);

    auto missing = scene::load_scene(f.w, temp_dir() / "does_not_exist.aescene");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().is(ErrorCode::NotFound));
    CHECK(f.w.entity_count() == 8); // failed load leaves the world untouched
}

TEST_CASE("malformed documents fail cleanly and never clear the world") {
    World        w;
    const Entity keep = w.create("Keep");
    auto         bad  = scene::load_scene_from_string(w, "{ not json");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().is(ErrorCode::InvalidArgument));
    CHECK_FALSE(scene::load_scene_from_string(w, "[]"));
    CHECK_FALSE(scene::load_scene_from_string(w, R"({"format":"other","version":1,"entities":[]})"));
    CHECK_FALSE(scene::load_scene_from_string(w, R"({"format":"aether.scene","entities":[]})"));
    CHECK_FALSE(scene::load_scene_from_string(w, R"({"format":"aether.scene","version":1})"));
    auto future = scene::load_scene_from_string(w, R"({"format":"aether.scene","version":99,"entities":[]})");
    REQUIRE_FALSE(future);
    CHECK(future.error().is(ErrorCode::Unsupported));
    CHECK(w.valid(keep));
    CHECK(w.entity_count() == 1);
}

TEST_CASE("unknown components, bad fields and dangling parents warn and are skipped") {
    const char* doc = R"({
      "format": "aether.scene", "version": 1,
      "entities": [
        { "uuid": "00000000000000aa",
          "components": { "Name": { "name": "Root" },
                          "Transform": { "position": [1, 2, 3], "rotation": "bogus" },
                          "Physics": { "mass": 5 },
                          "Light": { "kind": "Laser", "intensity": 2 } } },
        { "uuid": "00000000000000bb", "parent": "00000000000000aa",
          "components": { "Name": { "name": "Child" }, "Tag": { "tag": -1 } } },
        { "uuid": "00000000000000cc", "parent": "0000000000000fff",
          "components": { "Name": { "name": "Dangling" } } },
        { "parent": "00000000000000bb" },
        42
      ]
    })";
    World w;
    auto  r = scene::load_scene_from_string(w, doc);
    REQUIRE(r);
    CHECK(r->entity_count == 4);
    CHECK(r->warning_count == 6); // rotation, Physics, Light.kind, Tag.tag, dangling parent, 42
    const Entity root = scene::find_by_uuid(w, 0xaa);
    const Entity child = scene::find_by_uuid(w, 0xbb);
    REQUIRE(root != kNull);
    REQUIRE(child != kNull);
    CHECK(scene::parent_of(w, child) == root);
    CHECK(scene::local_transform(w, root).position == Vec3(1, 2, 3));
    CHECK(scene::local_transform(w, root).rotation == Quat(1, 0, 0, 0)); // default kept
    CHECK(w.get<LightComponent>(root).kind == LightKind::Point);        // default kept
    CHECK(w.get<LightComponent>(root).intensity == 2.0f);
    CHECK(w.get<TagComponent>(child).tag == 0u);
    CHECK(scene::parent_of(w, scene::find_by_uuid(w, 0xcc)) == kNull);
    CHECK(scene::child_count_of(w, child) == 1); // uuid-less entity still linked, fresh uuid
    check_valid(w);
}

TEST_CASE("hand-authored order: children before parents still link in document order") {
    const char* doc = R"({
      "format": "aether.scene", "version": 1,
      "entities": [
        { "uuid": "0000000000000002", "parent": "0000000000000001", "components": { "Name": { "name": "B" } } },
        { "uuid": "0000000000000003", "parent": "0000000000000001", "components": { "Name": { "name": "C" } } },
        { "uuid": "0000000000000001", "components": { "Name": { "name": "A" } } },
        { "uuid": "0000000000000004", "parent": "0000000000000005", "components": {} },
        { "uuid": "0000000000000005", "parent": "0000000000000004", "components": {} }
      ]
    })";
    World w;
    auto  r = scene::load_scene_from_string(w, doc);
    REQUIRE(r);
    CHECK(r->warning_count == 1); // the 4 <-> 5 cycle
    const Entity a = scene::find_by_uuid(w, 1);
    CHECK(checked_children(w, a) ==
          std::vector<Entity>{ scene::find_by_uuid(w, 2), scene::find_by_uuid(w, 3) });
    check_valid(w);
}

TEST_CASE("additive loads re-key colliding uuids; keep_uuids=false instantiates fresh") {
    Fixture f;
    auto    text = scene::save_scene_to_string(f.w);
    REQUIRE(text);
    auto r = scene::load_scene_from_string(f.w, *text, { .clear_world = false, .keep_uuids = true });
    REQUIRE(r);
    CHECK(f.w.entity_count() == 16);
    CHECK(r->warning_count == 8); // every uuid collided
    check_valid(f.w);

    auto r2 = scene::load_scene_from_string(f.w, *text, { .clear_world = false, .keep_uuids = false });
    REQUIRE(r2);
    CHECK(r2->warning_count == 0);
    CHECK(f.w.entity_count() == 24);
    CHECK(scene::root_count(f.w) == 9);
    check_valid(f.w);
}

TEST_CASE("clone_entity deep-copies a subtree with fresh uuids") {
    Fixture      f;
    const Entity foreign_target = f.body;
    f.w.add<ForeignComponent>(foreign_target, ForeignComponent{ 7, true });

    const Entity copy = scene::clone_entity(f.w, f.player);
    REQUIRE(copy != kNull);
    CHECK(f.w.entity_count() == 8 + 5);
    // Inserted right after the source among the roots.
    CHECK(checked_children(f.w, kNull) == std::vector<Entity>{ f.sun, f.player, copy, f.prop });

    std::vector<Entity> src, dst;
    src.push_back(f.player);
    dst.push_back(copy);
    scene::for_each_descendant(f.w, f.player, [&](Entity e) { src.push_back(e); });
    scene::for_each_descendant(f.w, copy, [&](Entity e) { dst.push_back(e); });
    REQUIRE(src.size() == dst.size());
    std::set<u64> uuids;
    for (usize i = 0; i < src.size(); ++i) {
        CHECK(f.w.get<NameComponent>(src[i]).name == f.w.get<NameComponent>(dst[i]).name);
        CHECK(scene::uuid_of(f.w, src[i]) != scene::uuid_of(f.w, dst[i]));
        CHECK(scene::find_by_uuid(f.w, scene::uuid_of(f.w, dst[i])) == dst[i]);
        uuids.insert(scene::uuid_of(f.w, dst[i]));
        CHECK(scene::child_count_of(f.w, src[i]) == scene::child_count_of(f.w, dst[i]));
        CHECK(scene::path_of(f.w, src[i]).size() == scene::path_of(f.w, dst[i]).size());
    }
    CHECK(uuids.size() == src.size());
    const Entity body_copy = dst[1];
    REQUIRE(f.w.has<ForeignComponent>(body_copy));
    CHECK(f.w.get<ForeignComponent>(body_copy).value == 7);
    CHECK(f.w.get<MeshRendererComponent>(body_copy).mesh == f.w.get<MeshRendererComponent>(f.body).mesh);
    CHECK(f.w.get<TagComponent>(copy).tag == 0xDEADBEEFu);
    CHECK_FALSE(scene::is_visible_in_hierarchy(f.w, dst[2])); // hidden Head stays hidden
    f.w.update_transforms();
    CHECK(mat_near(f.w.world_matrix(dst.back()), f.w.world_matrix(src.back())));

    // Clone under a new parent (appended, transforms relative to it).
    const Entity under = scene::clone_entity(f.w, f.body, f.prop);
    CHECK(scene::parent_of(f.w, under) == f.prop);
    CHECK(scene::last_child_of(f.w, f.prop) == under);
    CHECK(scene::clone_entity(f.w, kNull) == kNull);
    check_valid(f.w);
}

TEST_CASE("subtree copy/paste and prefab-style files") {
    Fixture f;
    auto    clip = scene::save_subtree_to_string(f.w, f.body);
    REQUIRE(clip);
    CHECK(clip->find("\"kind\": \"entities\"") != std::string::npos);

    // Paste into the same world under another parent: fresh uuids, same structure.
    auto pasted = scene::load_subtree_from_string(f.w, *clip, f.prop);
    REQUIRE(pasted);
    CHECK(scene::parent_of(f.w, *pasted) == f.prop);
    CHECK(f.w.get<NameComponent>(*pasted).name == "Body");
    CHECK(scene::uuid_of(f.w, *pasted) != scene::uuid_of(f.w, f.body));
    const Entity head = scene::first_child_of(f.w, *pasted);
    REQUIRE(head != kNull);
    CHECK(f.w.get<NameComponent>(head).name == "Head");
    CHECK_FALSE(scene::is_visible_in_hierarchy(f.w, head));
    check_valid(f.w);

    // Multi-selection: nested selections are folded into their ancestor.
    const Entity sel[] = { f.player, f.body, f.sun };
    auto         multi = scene::save_entities_to_string(f.w, sel);
    REQUIRE(multi);
    World other;
    auto  roots = scene::load_entities_from_string(other, *multi);
    REQUIRE(roots);
    CHECK(roots->size() == 2);
    CHECK(other.entity_count() == 6); // player subtree (5) + sun
    check_valid(other);

    // Prefab file into another world.
    const auto file = temp_dir() / "body.aeprefab";
    REQUIRE(scene::save_subtree(f.w, f.body, file));
    World     w3;
    const Entity anchor = w3.create("Anchor");
    auto      inst   = scene::load_subtree(w3, file, anchor);
    REQUIRE(inst);
    CHECK(scene::path_of(w3, scene::first_child_of(w3, *inst)) == "Anchor/Body/Head");

    const Entity none[] = { kNull };
    CHECK_FALSE(scene::save_entities_to_string(f.w, none));
    const Entity dead = f.w.create("Dead");
    f.w.destroy(dead);
    CHECK_FALSE(scene::load_subtree_from_string(f.w, *clip, dead)); // invalid paste parent
}

TEST_CASE("component codecs serialize components owned by other modules") {
    Fixture f;
    f.w.add<ForeignComponent>(f.body, ForeignComponent{ 42, true });

    scene::ComponentCodec codec;
    codec.name = "Foreign";
    codec.save = [](const World& w, Entity e, std::string& out) {
        const auto* c = w.try_get<ForeignComponent>(e);
        if (c == nullptr) {
            return false;
        }
        out = std::format(R"({{"value":{},"flag":{}}})", c->value, c->flag ? "true" : "false");
        return true;
    };
    codec.load = [](World& w, Entity e, StringView json) -> Result<void> {
        int value = 0;
        if (std::sscanf(std::string(json).c_str(), R"({"value":%d)", &value) != 1) {
            return make_error(ErrorCode::InvalidArgument, "bad Foreign payload");
        }
        // Deferred until the hierarchy exists: the parent is already linked.
        CHECK(scene::parent_of(w, e) != kNull);
        w.add<ForeignComponent>(e, ForeignComponent{ value, json.find("true") != StringView::npos });
        return {};
    };
    CHECK_FALSE(scene::register_component_codec(f.w, { "Transform", codec.save, codec.load }));
    CHECK_FALSE(scene::register_component_codec(f.w, { "", codec.save, codec.load }));
    REQUIRE(scene::register_component_codec(f.w, codec));

    auto text = scene::save_scene_to_string(f.w);
    REQUIRE(text);
    CHECK(text->find("\"Foreign\"") != std::string::npos);

    // PIE-style restore into the same world: codecs survive clear().
    f.w.get<ForeignComponent>(f.body).value = -1;
    auto r = scene::load_scene_from_string(f.w, *text);
    REQUIRE(r);
    CHECK(r->warning_count == 0);
    const Entity body = scene::find_by_name(f.w, "Body");
    REQUIRE(f.w.has<ForeignComponent>(body));
    CHECK(f.w.get<ForeignComponent>(body).value == 42);
    CHECK(f.w.get<ForeignComponent>(body).flag);

    // A world without the codec warns and skips the component.
    World plain;
    auto  r2 = scene::load_scene_from_string(plain, *text);
    REQUIRE(r2);
    CHECK(r2->warning_count == 1);

    // Failing loads warn; invalid saved JSON omits the component but still saves the scene.
    std::string bad = *text;
    bad.replace(bad.find("\"value\": 42"), 11, "\"value\": \"x\"");
    auto r3 = scene::load_scene_from_string(f.w, bad);
    REQUIRE(r3);
    CHECK(r3->warning_count == 1);
    codec.save = [](const World&, Entity, std::string& out) { out = "{not json"; return true; };
    REQUIRE(scene::register_component_codec(f.w, codec)); // replaces by name
    auto text2 = scene::save_scene_to_string(f.w);
    REQUIRE(text2);
    CHECK(text2->find("\"Foreign\"") == std::string::npos);
    scene::unregister_component_codec(f.w, "Foreign");
    auto r4 = scene::load_scene_from_string(f.w, *text);
    REQUIRE(r4);
    CHECK(r4->warning_count == 1); // codec gone -> unknown component
}
