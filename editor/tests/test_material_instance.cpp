// test_material_instance.cpp — scene-stored material instances: ids, binding, JSON, codec.
#include "aether/editor/material_instance.h"
#include "aether/editor/prefab.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::editor;

namespace {
assets::MaterialData sample_material() {
    assets::MaterialData m;
    m.name               = "Brick";
    m.base_color_factor  = Vec4(0.8f, 0.3f, 0.2f, 1.0f);
    m.emissive_factor    = Vec3(0.0f, 2.5f, 0.0f);
    m.metallic_factor    = 0.1f;
    m.roughness_factor   = 0.7f;
    m.normal_scale       = 0.5f;
    m.occlusion_strength = 0.25f;
    m.alpha_cutoff       = 0.4f;
    m.alpha_mode         = assets::AlphaMode::Mask;
    m.double_sided       = true;
    m.normal_texture     = AssetId{ 0x0123456789abcdefull, 0xfedcba9876543210ull };
    return m;
}

void check_same(const assets::MaterialData& a, const assets::MaterialData& b) {
    CHECK(a.name == b.name);
    CHECK(a.base_color_factor == b.base_color_factor);
    CHECK(a.emissive_factor == b.emissive_factor);
    CHECK(a.metallic_factor == b.metallic_factor);
    CHECK(a.roughness_factor == b.roughness_factor);
    CHECK(a.normal_scale == b.normal_scale);
    CHECK(a.occlusion_strength == b.occlusion_strength);
    CHECK(a.alpha_cutoff == b.alpha_cutoff);
    CHECK(a.alpha_mode == b.alpha_mode);
    CHECK(a.double_sided == b.double_sided);
    CHECK(a.normal_texture == b.normal_texture);
    CHECK(a.base_color_texture == b.base_color_texture);
    CHECK(material_data_hash(a) == material_data_hash(b));
}

Entity make_mesh_entity(World& w, const char* name) {
    const Entity          e = w.create(name);
    MeshRendererComponent mr;
    mr.mesh     = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Cube);
    mr.material = gameplay::default_material_id();
    w.add<MeshRendererComponent>(e, mr);
    return e;
}
} // namespace

TEST_CASE("Material instance: ids, asset id parsing and hashing") {
    const AssetId a = material_instance_id(42, -1);
    CHECK(a == material_instance_id(42, -1)); // deterministic
    CHECK(a != material_instance_id(43, -1));
    CHECK(a != material_instance_id(42, 0));
    CHECK(is_material_instance_id(a));
    CHECK(is_material_instance_id(material_instance_id(7, 3)));
    CHECK_FALSE(is_material_instance_id(gameplay::default_material_id()));
    CHECK_FALSE(is_material_instance_id(AssetId{}));

    AssetId parsed;
    const AssetId src{ 0x0123456789abcdefull, 0xfedcba9876543210ull };
    REQUIRE(parse_asset_id(src.to_string(), parsed));
    CHECK(parsed == src);
    CHECK(parse_asset_id("", parsed));
    CHECK_FALSE(parsed.is_valid());
    CHECK_FALSE(parse_asset_id("xyz", parsed));
    CHECK_FALSE(parse_asset_id("0123456789abcdef0123456789abcdeg", parsed));

    assets::MaterialData m = sample_material();
    const u64            h = material_data_hash(m);
    m.roughness_factor     = 0.71f;
    CHECK(material_data_hash(m) != h);
}

TEST_CASE("Material instance: JSON round trip is exact") {
    const assets::MaterialData m = sample_material();
    auto                       r = material_from_json(material_to_json(m));
    REQUIRE(r.has_value());
    check_same(*r, m);
    CHECK(!material_from_json("[1,2]").has_value());
    CHECK(!material_from_json("not json").has_value());
}

TEST_CASE("Material instance: binding follows uuids (duplicates get their own id)") {
    World        w;
    const Entity e = make_mesh_entity(w, "E");
    make_material_instance(w, e, kPrimaryMaterialSlot, sample_material());
    make_material_instance(w, e, 2, sample_material());
    const u64 uuid = scene::uuid_of(w, e);
    CHECK(w.get<MeshRendererComponent>(e).material == material_instance_id(uuid, -1));
    REQUIRE(w.has<gameplay::MaterialOverridesComponent>(e));
    CHECK(w.get<gameplay::MaterialOverridesComponent>(e).materials.size() == 3);
    CHECK(material_slot_target(w, e, 2) == material_instance_id(uuid, 2));
    CHECK_FALSE(material_slot_target(w, e, 0).is_valid());
    CHECK(bind_material_instances(w) == 0); // already bound

    const Entity copy = scene::clone_entity(w, e); // copies the component and the stale references
    CHECK(w.get<MeshRendererComponent>(copy).material == material_instance_id(uuid, -1));
    CHECK(bind_material_instances(w) == 2);
    const u64 cu = scene::uuid_of(w, copy);
    CHECK(w.get<MeshRendererComponent>(copy).material == material_instance_id(cu, -1));
    CHECK(material_slot_target(w, copy, 2) == material_instance_id(cu, 2));

    // An instance slot on an entity without a mesh renderer is left alone (no endless re-binding).
    const Entity bare = w.create("Bare");
    w.add<MaterialInstanceComponent>(bare).slots.push_back({ kPrimaryMaterialSlot, {} });
    CHECK(bind_material_instances(w) == 0);

    remove_material_instance(w, e, kPrimaryMaterialSlot, gameplay::default_material_id());
    CHECK(w.get<MeshRendererComponent>(e).material == gameplay::default_material_id());
    remove_material_instance(w, e, 2, AssetId{});
    CHECK_FALSE(w.has<MaterialInstanceComponent>(e));
    CHECK_FALSE(material_slot_target(w, e, 2).is_valid());
}

TEST_CASE("Material instance: codec round-trips through scene files") {
    World w;
    REQUIRE(register_editor_codecs(w));
    const Entity e = make_mesh_entity(w, "E");
    make_material_instance(w, e, kPrimaryMaterialSlot, sample_material());
    auto text = scene::save_scene_to_string(w);
    REQUIRE(text.has_value());

    World dst;
    REQUIRE(register_editor_codecs(dst));
    REQUIRE(scene::load_scene_from_string(dst, *text).has_value());
    const Entity d = scene::find_by_uuid(dst, scene::uuid_of(w, e));
    REQUIRE((d != kNullEntity));
    const auto* mi = dst.try_get<MaterialInstanceComponent>(d);
    REQUIRE(mi != nullptr);
    REQUIRE(mi->find(kPrimaryMaterialSlot) != nullptr);
    check_same(mi->find(kPrimaryMaterialSlot)->data, sample_material());
    CHECK(bind_material_instances(dst) == 0); // uuids kept: references still valid
}
