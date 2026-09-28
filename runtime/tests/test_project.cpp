// test_project.cpp — .aeproject manifests, project discovery and packaging.
#include "aether/assets/asset_manager.h"
#include "aether/core/job_system.h"
#include "aether/gameplay/application.h"
#include "aether/runtime/project.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::runtime;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("aether_runtime_" + std::string(tag) + "_" + std::to_string(stamp) + "_" +
                                            std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path operator/(const fs::path& p) const { return path / p; }
};

void write_text(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read_text(const fs::path& file) {
    std::ifstream     in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const fs::path kContent = AE_TEST_CONTENT_DIR;

} // namespace

TEST_CASE("project: minimal manifest uses defaults") {
    auto p = parse_project(R"({ "format": "aether.project" })", "/base");
    REQUIRE_MESSAGE(p.has_value(), p.error().message);
    CHECK(p->name == "Aether Game");
    CHECK(p->window.title == "Aether Game");
    CHECK(p->window.width == 1280);
    CHECK(p->startup_scene.empty());
    CHECK(p->content_root.empty());
    CHECK_FALSE(p->cooked_assets);
    CHECK(p->camera_controller);
    CHECK(p->sky);
}

TEST_CASE("project: full manifest parses and resolves roots against the base directory") {
    const char* text = R"({
        // comments are allowed
        "format": "aether.project", "version": 1, "name": "Demo",
        "startup_scene": "scenes/level.aescene",
        "content_root": "content", "cooked_root": "../cooked", "cooked_assets": true,
        "input_map": "input/game.json",
        "window": { "title": "Demo Window", "width": 800, "height": 600, "resizable": false,
                    "vsync": false, "fullscreen": true },
        "simulation": { "fixed_delta": 0.02, "max_fixed_steps": 4 },
        "rendering": { "exposure": 1.5, "sky": false },
        "camera_controller": false,
        "future_key": 42
    })";
    const fs::path base = fs::path("/games/demo");
    auto           p    = parse_project(text, base);
    REQUIRE_MESSAGE(p.has_value(), p.error().message);
    CHECK(p->name == "Demo");
    CHECK(p->startup_scene == fs::path("scenes/level.aescene"));
    CHECK(p->content_root == (base / "content").lexically_normal());
    CHECK(p->cooked_root == fs::path("/games/cooked").lexically_normal());
    CHECK(p->cooked_assets);
    CHECK(p->input_map == fs::path("input/game.json"));
    CHECK(p->window.title == "Demo Window");
    CHECK(p->window.width == 800);
    CHECK(p->window.height == 600);
    CHECK_FALSE(p->window.resizable);
    CHECK_FALSE(p->window.vsync);
    CHECK(p->fullscreen);
    CHECK(p->fixed_delta == doctest::Approx(0.02f));
    CHECK(p->max_fixed_steps == 4);
    CHECK(p->exposure == doctest::Approx(1.5f));
    CHECK_FALSE(p->sky);
    CHECK_FALSE(p->camera_controller);
}

TEST_CASE("project: invalid manifests are rejected") {
    CHECK_FALSE(parse_project("not json", "/").has_value());
    CHECK_FALSE(parse_project("[]", "/").has_value());
    CHECK_FALSE(parse_project(R"({ "name": "no format" })", "/").has_value());
    CHECK_FALSE(parse_project(R"({ "format": "aether.scene" })", "/").has_value());
    CHECK(parse_project(R"({ "format": "aether.project", "version": 99 })", "/").error().code == ErrorCode::Unsupported);
    CHECK_FALSE(parse_project(R"({ "format": "aether.project", "name": 5 })", "/").has_value());
    CHECK_FALSE(parse_project(R"({ "format": "aether.project", "window": { "width": 0 } })", "/").has_value());
    CHECK_FALSE(parse_project(R"({ "format": "aether.project", "simulation": { "fixed_delta": 0 } })", "/").has_value());
    CHECK_FALSE(parse_project(R"({ "format": "aether.project", "rendering": { "exposure": -1 } })", "/").has_value());
}

TEST_CASE("project: to_json round-trips and writes roots relative to the base") {
    ProjectDesc p;
    p.name            = "Round Trip";
    p.startup_scene   = "scenes/a.aescene";
    p.content_root    = fs::path("/pkg/content");
    p.cooked_root     = fs::path("/elsewhere/assets");
    p.cooked_assets   = true;
    p.input_map       = "input.json";
    p.window.title    = "RT";
    p.window.width    = 640;
    p.window.height   = 480;
    p.fullscreen      = true;
    p.fixed_delta     = 0.01f;
    p.max_fixed_steps = 3;
    p.exposure        = 2.0f;
    p.sky             = false;

    const std::string text = project_to_json(p, "/pkg");
    CHECK(text.find("\"content_root\": \"content\"") != std::string::npos);
    auto q = parse_project(text, "/pkg");
    REQUIRE_MESSAGE(q.has_value(), q.error().message);
    CHECK(q->name == p.name);
    CHECK(q->startup_scene == p.startup_scene);
    CHECK(q->content_root == p.content_root);
    CHECK(q->cooked_root == p.cooked_root);
    CHECK(q->cooked_assets);
    CHECK(q->input_map == p.input_map);
    CHECK(q->window.title == "RT");
    CHECK(q->window.width == 640);
    CHECK(q->fullscreen);
    CHECK(q->fixed_delta == doctest::Approx(0.01f));
    CHECK(q->max_fixed_steps == 3);
    CHECK(q->exposure == doctest::Approx(2.0f));
    CHECK_FALSE(q->sky);
}

TEST_CASE("project: apply_project fills the application settings") {
    ProjectDesc p;
    p.window.title   = "Applied";
    p.window.width   = 1024;
    p.window.vsync   = false;
    p.fixed_delta    = 0.025f;
    p.content_root   = "/c";
    p.cooked_root    = "/k";
    p.cooked_assets  = true;
    p.startup_scene  = "s.aescene";
    gameplay::AppDesc d;
    apply_project(p, d);
    CHECK(d.window.title == "Applied");
    CHECK(d.window.width == 1024);
    CHECK_FALSE(d.vsync);
    CHECK(d.fixed_delta == doctest::Approx(0.025f));
    CHECK(d.content_root == fs::path("/c"));
    CHECK(d.cooked_root == fs::path("/k"));
    CHECK(d.cooked_assets_only);
    CHECK(d.startup_scene == fs::path("s.aescene"));
}

TEST_CASE("project: default project discovery") {
    TempDir exe("exe");
    TempDir root("root");
    CHECK(find_default_project(exe.path, root.path).empty());

    write_text(root / "zeta.aeproject", "{}");
    CHECK(find_default_project(exe.path, root.path) == root / "zeta.aeproject");
    write_text(root / "game.aeproject", "{}");
    CHECK(find_default_project(exe.path, root.path) == root / "game.aeproject"); // preferred name

    write_text(exe / "b.AEPROJECT", "{}"); // next to the executable wins; case-insensitive
    write_text(exe / "a.aeproject", "{}");
    write_text(exe / "notes.txt", "");
    CHECK(find_default_project(exe.path, root.path) == exe / "a.aeproject");
}

TEST_CASE("project: runtime content filter") {
    CHECK(is_runtime_content_file("scenes/a.aescene"));
    CHECK(is_runtime_content_file("prefabs/b.aeprefab"));
    CHECK(is_runtime_content_file("scripts/c.LUA"));
    CHECK(is_runtime_content_file("scripts/graphs/d.aegraph")); // ADR-0018 visual scripts
    CHECK(is_runtime_content_file("input/game.json"));
    CHECK_FALSE(is_runtime_content_file("samples/cube/cube.gltf"));
    CHECK_FALSE(is_runtime_content_file("textures/t.png"));
    CHECK_FALSE(is_runtime_content_file("tools/generate.py"));
}

TEST_CASE("package: cooks, copies runtime content and loads cooked-only") {
    JobSystem::initialize(2);
    TempDir dev("dev");
    TempDir out("out");

    // A small development tree: a glTF source, a scene, a script and an unrelated file.
    fs::create_directories(dev / "content/models");
    fs::copy_file(kContent / "samples/cube/cube.glb", dev / "content/models/cube.glb");
    write_text(dev / "content/scenes/level.aescene", R"({ "format": "aether.scene", "version": 1, "entities": [] })");
    write_text(dev / "content/scripts/hello.lua", "function on_update(self, dt) end\n");
    write_text(dev / "content/tools/notes.py", "print('dev only')\n");
    write_text(dev / "shaders/common/x.glsl", "// shader\n");
    write_text(dev / "fake-player", "binary");
    write_text(dev / "dev.aeproject", R"({ "format": "aether.project", "name": "Pkg",
        "startup_scene": "scenes/level.aescene", "content_root": "content", "cooked_root": "cooked" })");

    PackageOptions o;
    o.project_file = dev / "dev.aeproject";
    o.output_dir   = out.path;
    o.executable   = dev / "fake-player";
    o.shader_dir   = dev / "shaders";
    auto r = package_project(o);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK(r->sources_failed == 0);
    CHECK(r->sources_cooked == 1);
    CHECK(r->assets > 0);
    CHECK(r->files_copied == 4); // scene + script + shader + player
    CHECK(fs::exists(out / "assets/asset_db.json"));
    CHECK(fs::exists(out / "content/scenes/level.aescene"));
    CHECK(fs::exists(out / "content/scripts/hello.lua"));
    CHECK(fs::exists(out / "shaders/common/x.glsl"));
    CHECK(fs::exists(out / "fake-player"));
    CHECK_FALSE(fs::exists(out / "content/models/cube.glb")); // sources are cooked, not shipped
    CHECK_FALSE(fs::exists(out / "content/tools/notes.py"));
    CHECK_FALSE(fs::exists(dev / "cooked")); // the dev tree is untouched

    // The packaged manifest is relocatable and ships cooked data only.
    REQUIRE(r->manifest == out / "game.aeproject");
    const std::string manifest = read_text(r->manifest);
    CHECK(manifest.find("\"content_root\": \"content\"") != std::string::npos);
    CHECK(manifest.find("\"cooked_root\": \"assets\"") != std::string::npos);
    auto shipped = load_project(r->manifest);
    REQUIRE(shipped.has_value());
    CHECK(shipped->cooked_assets);
    CHECK(shipped->name == "Pkg");
    CHECK(shipped->startup_scene == fs::path("scenes/level.aescene"));

    // A Runtime-mode AssetManager loads the model from the package without its source.
    {
        assets::AssetManagerConfig cfg;
        cfg.content_root = shipped->content_root;
        cfg.cooked_root  = shipped->cooked_root;
        cfg.mode         = assets::AssetLoadMode::Runtime;
        assets::AssetManager mgr;
        REQUIRE(mgr.initialize(cfg).has_value());
        auto mesh = mgr.load_sync<assets::MeshData>(assets::make_asset_id("models/cube.glb", "mesh:0"));
        REQUIRE_MESSAGE(mesh.ready(), mesh.error().message);
        CHECK(mesh->vertices.size() == 24);
    }

    // Packaging again is incremental (nothing re-cooked) and still succeeds.
    auto again = package_project(o);
    REQUIRE(again.has_value());
    CHECK(again->sources_failed == 0);
    CHECK(again->assets == r->assets);

    // A missing startup scene is an error.
    write_text(dev / "bad.aeproject", R"({ "format": "aether.project", "startup_scene": "nope.aescene",
        "content_root": "content" })");
    o.project_file = dev / "bad.aeproject";
    CHECK(package_project(o).error().code == ErrorCode::NotFound);
    JobSystem::shutdown();
}
