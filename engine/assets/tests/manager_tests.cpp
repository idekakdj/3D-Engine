// manager_tests.cpp — AssetManager async/sync loads, lifetime, runtime mode, hot reload.
#include "aether/assets/asset_manager.h"

#include "test_helpers.h"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::assets;
using namespace aether::assets::test;

namespace {
AssetManagerConfig editor_config(const TempDir& content, const TempDir& cooked) {
    AssetManagerConfig c;
    c.content_root = content.path();
    c.cooked_root = cooked.path();
    c.mode = AssetLoadMode::Editor;
    c.hot_reload_poll_interval = 0.0;
    return c;
}

String read_all(const fs::path& p) {
    std::ifstream     in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("manager: async load, wait_all, sharing, sync load, failures") {
    ensure_job_system();
    TempDir content("mgr_content");
    TempDir cooked("mgr_cooked");
    copy_into(sample("cube/cube.gltf"), content / "samples/cube/cube.gltf");
    copy_into(sample("skinned/skinned.gltf"), content / "samples/skinned/skinned.gltf");

    AssetManager mgr;
    REQUIRE(mgr.initialize(editor_config(content, cooked)).has_value());

    // Load by path (the source is imported on demand in Editor mode).
    AssetRef<MeshData> mesh = mgr.load<MeshData>("samples/cube/cube.gltf", "mesh:0");
    REQUIRE(mesh.valid());
    CHECK(mesh.id() == make_asset_id("samples/cube/cube.gltf", "mesh:0"));
    AssetRef<SceneData> scene = mgr.load<SceneData>("samples/skinned/skinned.gltf"); // primary asset
    mgr.wait_all();
    REQUIRE_MESSAGE(mesh.ready(), mesh.error().message);
    REQUIRE(mesh.get() != nullptr);
    CHECK(mesh->vertices.size() == 24);
    CHECK(mesh.version() == 1);
    REQUIRE(scene.ready());
    CHECK(scene->nodes.size() == 5);
    CHECK(mgr.state(mesh.id()) == AssetState::Ready);

    // Same id -> same shared entry, no reload.
    AssetRef<MeshData> again = mgr.load<MeshData>(mesh.id());
    CHECK(again == mesh);
    CHECK(again.version() == 1);
    CHECK(again.get_shared().get() == mesh.get());

    // Synchronous load of a referenced asset.
    const AssetId mat_id = scene->nodes[4].materials.at(0);
    auto          mat = mgr.load_sync<MaterialData>(mat_id);
    REQUIRE(mat.ready());
    CHECK(mat->name == "BarMaterial");
    auto skel = mgr.load_sync<SkeletonData>("samples/skinned/skinned.gltf", "skeleton:0");
    REQUIRE(skel.ready());
    CHECK(skel->joint_names.size() == 3);

    // Failures: unknown id, type mismatch.
    auto unknown = mgr.load_sync<MeshData>(make_asset_id("nope.gltf", "mesh:0"));
    CHECK(unknown.failed());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    CHECK(unknown.get() == nullptr);
    auto wrong = mgr.load<TextureData>(mesh.id());
    CHECK(wrong.failed());

    // Unload / collect.
    mgr.unload(mesh.id());
    CHECK(mesh.state() == AssetState::Unloaded);
    CHECK(mesh.get() == nullptr);
    auto reloaded = mgr.load_sync<MeshData>(mesh.id());
    CHECK(reloaded.ready());
    CHECK(reloaded.version() == 2);
    const usize before = mgr.registered_count();
    unknown = {};
    CHECK(mgr.collect_unused() == 1);
    CHECK(mgr.registered_count() == before - 1);

    mgr.shutdown();
    CHECK(fs::exists(cooked / kAssetDbFileName)); // editor mode persists the database
}

TEST_CASE("manager: runtime mode loads cooked data only") {
    ensure_job_system();
    TempDir content("rt_content");
    TempDir cooked("rt_cooked");
    copy_into(sample("cube/cube.gltf"), content / "samples/cube/cube.gltf");
    {
        AssetDatabase db;
        REQUIRE(db.open(content.path(), cooked.path()).has_value());
        CHECK(db.scan().imported == 1);
        REQUIRE(db.save().has_value());
    }
    fs::remove_all(content.path()); // the runtime never touches sources

    AssetManagerConfig cfg;
    cfg.content_root = content.path();
    cfg.cooked_root = cooked.path();
    cfg.mode = AssetLoadMode::Runtime;
    AssetManager mgr;
    REQUIRE(mgr.initialize(cfg).has_value());
    auto tex = mgr.load<TextureData>(make_asset_id("samples/cube/cube.gltf", "texture:0:srgb"));
    auto mat = mgr.load<MaterialData>(make_asset_id("samples/cube/cube.gltf", "material:0"));
    mgr.wait_all();
    REQUIRE_MESSAGE(tex.ready(), tex.error().message);
    CHECK(tex->width == 4);
    REQUIRE(mat.ready());
    CHECK(mat->base_color_texture == tex.id());
    mgr.poll_file_changes(); // no-op in Runtime mode
}

TEST_CASE("manager: hot reload swaps data on the main thread and fires callbacks") {
    ensure_job_system();
    TempDir content("hot_content");
    TempDir cooked("hot_cooked");
    const fs::path file = content / "cube.gltf";
    copy_into(sample("cube/cube.gltf"), file);

    AssetManager mgr;
    REQUIRE(mgr.initialize(editor_config(content, cooked)).has_value());
    auto mat = mgr.load_sync<MaterialData>("cube.gltf", "material:0");
    auto mesh = mgr.load_sync<MeshData>("cube.gltf", "mesh:0");
    REQUIRE(mat.ready());
    REQUIRE(mesh.ready());
    CHECK(mat->roughness_factor == doctest::Approx(0.5f));

    std::vector<AssetId> reloaded;
    const ListenerId     listener = mgr.on_reloaded([&](AssetId id) { reloaded.push_back(id); });

    mgr.poll_file_changes(); // nothing changed yet
    mgr.wait_all();
    mgr.poll_file_changes();
    CHECK(reloaded.empty());

    // Edit the source: roughness 0.5 -> 0.25.
    String text = read_all(file);
    const auto at = text.find("\"roughnessFactor\": 0.5");
    REQUIRE(at != String::npos);
    text.replace(at, 22, "\"roughnessFactor\": 0.25");
    write_text(file, text);
    bump_mtime(file);

    const MaterialData* old_ptr = mat.get();
    mgr.poll_file_changes(); // detects the change, re-imports on a job
    mgr.wait_all();
    CHECK(mat.version() == 1);          // not swapped until the next main-thread poll
    CHECK(mat.get() == old_ptr);
    mgr.poll_file_changes(); // applies
    CHECK(mat.version() == 2);
    CHECK(mat->roughness_factor == doctest::Approx(0.25f));
    CHECK(mesh.version() == 2);
    CHECK(std::find(reloaded.begin(), reloaded.end(), mat.id()) != reloaded.end());
    CHECK(std::find(reloaded.begin(), reloaded.end(), mesh.id()) != reloaded.end());

    // A broken edit keeps the last good data and is not retried until the file changes.
    reloaded.clear();
    write_text(file, "{ broken");
    bump_mtime(file, 10);
    mgr.poll_file_changes();
    mgr.wait_all();
    mgr.poll_file_changes();
    CHECK(reloaded.empty());
    CHECK(mat.ready());
    CHECK(mat->roughness_factor == doctest::Approx(0.25f));

    mgr.remove_reload_listener(listener);
    mgr.shutdown();
}
