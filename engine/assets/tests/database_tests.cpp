// database_tests.cpp — asset_db.json persistence, incremental cooking, pruning, deps.
#include "aether/assets/asset_database.h"
#include "aether/assets/format.h"

#include "test_helpers.h"

#include <doctest/doctest.h>
#include <stb_image_write.h> // declarations only; the implementation lives in import_tests.cpp

#include <algorithm>
#include <format>
#include <fstream>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::assets;
using namespace aether::assets::test;

namespace {

void write_small_png(const fs::path& path) {
    const u8        px[16] = { 1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255 };
    std::vector<u8> encoded;
    auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    REQUIRE(stbi_write_png_to_func(sink, &encoded, 2, 2, 4, px, 8) != 0);
    write_text(path, std::string(encoded.begin(), encoded.end()));
}

void append_text(const fs::path& path, const char* text) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << text;
}

const ImportOutcome* outcome_for(const ScanReport& r, const char* source) {
    for (const ImportOutcome& oc : r.outcomes) {
        if (oc.source_path == source) return &oc;
    }
    return nullptr;
}

} // namespace

TEST_CASE("database: scan, persist, incremental skip, prune") {
    ensure_job_system();
    TempDir content("db_content");
    TempDir cooked("db_cooked");
    copy_into(sample("cube/cube.gltf"), content / "samples/cube/cube.gltf");
    copy_into(sample("skinned/skinned.gltf"), content / "samples/skinned/skinned.gltf");
    write_small_png(content / "textures/albedo.png");
    write_text(content / "notes.txt", "ignored");

    AssetDatabase db;
    REQUIRE(db.open(content.path(), cooked.path()).has_value());
    const ScanReport first = db.scan(); // parallel on the JobSystem
    for (const ImportOutcome& oc : first.outcomes) CHECK_MESSAGE(oc.status != ImportStatus::Failed, oc.error.message);
    CHECK(first.sources_found == 3);
    CHECK(first.imported == 3);
    CHECK(first.failed == 0);
    // cube: mesh + material + 2 textures + scene; skinned: mesh + material + skeleton +
    // clip + scene; png: 1 texture.
    CHECK(first.assets_written == 11);
    CHECK(db.asset_count() == 11);

    const AssetId mesh_id = make_asset_id("samples/cube/cube.gltf", "mesh:0");
    auto          rec = db.find(mesh_id);
    REQUIRE(rec.has_value());
    CHECK(rec->type == AssetType::Mesh);
    CHECK(rec->sub_key == "mesh:0");
    CHECK(rec->source_path == "samples/cube/cube.gltf");
    CHECK(rec->importer_version == kImporterVersion);
    CHECK(fs::exists(db.cooked_file(*rec)));
    auto cooked_mesh = read_asset<MeshData>(db.cooked_file(*rec));
    REQUIRE(cooked_mesh.has_value());
    CHECK(cooked_mesh->vertices.size() == 24);
    CHECK(db.find("samples/cube/cube.gltf", "material:0").has_value());
    CHECK(db.primary_asset("samples/cube/cube.gltf") == make_asset_id("samples/cube/cube.gltf", "scene:0"));
    CHECK(db.primary_asset("textures/albedo.png") == make_asset_id("textures/albedo.png", "texture:0"));
    CHECK(db.assets_of_source("samples/skinned/skinned.gltf").size() == 5);
    CHECK(db.stamps_match("samples/cube/cube.gltf"));

    REQUIRE(db.save().has_value());
    CHECK_FALSE(db.dirty());
    REQUIRE(fs::exists(cooked / kAssetDbFileName));

    SUBCASE("round trip through asset_db.json") {
        AssetDatabase reopened;
        REQUIRE(reopened.open(content.path(), cooked.path()).has_value());
        const auto a = db.all_assets();
        const auto b = reopened.all_assets();
        REQUIRE(a.size() == b.size());
        for (usize i = 0; i < a.size(); ++i) {
            CHECK(a[i].id == b[i].id);
            CHECK(a[i].type == b[i].type);
            CHECK(a[i].source_path == b[i].source_path);
            CHECK(a[i].sub_key == b[i].sub_key);
            CHECK(a[i].name == b[i].name);
            CHECK(a[i].cooked_path == b[i].cooked_path);
            CHECK(a[i].source_hash == b[i].source_hash);
        }
        CHECK(reopened.all_sources() == db.all_sources());
        CHECK(reopened.stamps_match("samples/skinned/skinned.gltf"));
        CHECK_FALSE(reopened.dirty());
    }

    SUBCASE("incremental: unchanged sources are skipped, changed ones re-imported") {
        const ScanReport again = db.scan();
        CHECK(again.up_to_date == 3);
        CHECK(again.imported == 0);

        append_text(content / "samples/cube/cube.gltf", "\n"); // content (and hash) change
        bump_mtime(content / "samples/cube/cube.gltf");
        CHECK_FALSE(db.stamps_match("samples/cube/cube.gltf"));
        const u64        old_hash = db.find_source("samples/cube/cube.gltf")->source_hash;
        const ScanReport changed = db.scan();
        CHECK(changed.imported == 1);
        CHECK(changed.up_to_date == 2);
        REQUIRE(outcome_for(changed, "samples/cube/cube.gltf") != nullptr);
        CHECK(outcome_for(changed, "samples/cube/cube.gltf")->status == ImportStatus::Imported);
        CHECK(db.find_source("samples/cube/cube.gltf")->source_hash != old_hash);
        CHECK(read_asset_header(db.cooked_file(*db.find(mesh_id)))->source_hash ==
              db.find_source("samples/cube/cube.gltf")->source_hash);

        // Touched but identical: skipped, stamps refreshed.
        bump_mtime(content / "samples/skinned/skinned.gltf");
        CHECK_FALSE(db.stamps_match("samples/skinned/skinned.gltf"));
        const ScanReport touched = db.scan(ScanOptions{ .parallel = false });
        CHECK(touched.up_to_date == 3);
        CHECK(db.stamps_match("samples/skinned/skinned.gltf"));

        // A missing cooked file forces a re-import.
        fs::remove(db.cooked_file(*db.find(mesh_id)));
        CHECK(db.scan().imported == 1);

        const ScanReport forced = db.scan(ScanOptions{ .force = true });
        CHECK(forced.imported == 3);
    }

    SUBCASE("deleted sources are pruned with their cooked files") {
        const auto png = db.find("textures/albedo.png", "texture:0");
        REQUIRE(png.has_value());
        const fs::path png_cooked = db.cooked_file(*png);
        REQUIRE(fs::exists(png_cooked));
        fs::remove(content / "textures/albedo.png");
        const ScanReport pruned = db.scan();
        CHECK(pruned.removed == 1);
        CHECK(pruned.sources_found == 2);
        CHECK(db.asset_count() == 10);
        CHECK_FALSE(db.find(png->id).has_value());
        CHECK_FALSE(fs::exists(png_cooked));
        CHECK(db.dirty());
    }
}

TEST_CASE("database: external buffer dependency drives re-import") {
    TempDir content("db_deps");
    TempDir cooked("db_deps_cooked");
    std::vector<u8> bin;
    append_f32(bin, { 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    write_text(content / "ext/tri.bin", std::string(bin.begin(), bin.end()));
    write_text(content / "ext/tri.gltf",
               R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":"tri.bin"}],
"bufferViews":[{"buffer":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})");

    AssetDatabase db;
    REQUIRE(db.open(content.path(), cooked.path()).has_value());
    ImportResult  result;
    ImportOutcome first = db.import_source(content / "ext/tri.gltf", false, &result);
    REQUIRE_MESSAGE(first.status == ImportStatus::Imported, first.error.message);
    CHECK(result.meshes.size() == 1);
    const auto source = db.find_source("ext/tri.gltf");
    REQUIRE(source.has_value());
    CHECK(source->dependencies == std::vector<String>{ "ext/tri.bin" });
    CHECK(db.import_source("ext/tri.gltf").status == ImportStatus::UpToDate); // relative path accepted

    // Change only the .bin: the source must be re-imported.
    bin[0] = 0x01;
    write_text(content / "ext/tri.bin", std::string(bin.begin(), bin.end()));
    bump_mtime(content / "ext/tri.bin");
    CHECK_FALSE(db.stamps_match("ext/tri.gltf"));
    CHECK(db.import_source("ext/tri.gltf").status == ImportStatus::Imported);
    CHECK(db.stamps_match("ext/tri.gltf"));
}

TEST_CASE("database: failed imports are reported and keep the previous cook") {
    TempDir content("db_fail");
    TempDir cooked("db_fail_cooked");
    copy_into(sample("cube/cube.gltf"), content / "cube.gltf");
    AssetDatabase db;
    REQUIRE(db.open(content.path(), cooked.path()).has_value());
    REQUIRE(db.import_source("cube.gltf").status == ImportStatus::Imported);
    const usize count = db.asset_count();

    write_text(content / "cube.gltf", "{ broken");
    bump_mtime(content / "cube.gltf");
    const ImportOutcome oc = db.import_source("cube.gltf");
    CHECK(oc.status == ImportStatus::Failed);
    CHECK_FALSE(oc.error.message.empty());
    CHECK(db.asset_count() == count); // last good cook stays available

    const ScanReport report = db.scan(ScanOptions{ .parallel = false });
    CHECK(report.failed == 1);
    CHECK(db.import_source("missing.gltf").error.code == ErrorCode::NotFound);
}
