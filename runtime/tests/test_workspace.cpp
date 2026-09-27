// test_workspace.cpp — user workspace of an installed Aether (ADR-0013).
#include "aether/runtime/project.h"
#include "aether/runtime/workspace.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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
        path = fs::temp_directory_path() /
               ("aether_ws_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& f, const std::string& text) {
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << text;
}

std::string read_text(const fs::path& f) {
    std::ifstream     in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void set_documents(const fs::path& p) {
#ifdef _WIN32
    _putenv_s("AETHER_DOCUMENTS_DIR", p.string().c_str());
#else
    setenv("AETHER_DOCUMENTS_DIR", p.string().c_str(), 1);
#endif
}

void clear_documents() {
#ifdef _WIN32
    _putenv_s("AETHER_DOCUMENTS_DIR", "");
#else
    unsetenv("AETHER_DOCUMENTS_DIR");
#endif
}

// A fake install root: shaders/ + content/ with a scene and a script.
void make_install(const fs::path& root, bool marker) {
    write_text(root / "shaders/renderer/x.glsl", "// shader\n");
    write_text(root / "content/scenes/showcase.aescene", R"({ "format": "aether.scene", "version": 1, "entities": [] })");
    write_text(root / "content/scripts/a.lua", "-- script\n");
    if (marker) {
        write_text(root / kInstallMarker, R"({ "product": "Aether" })");
    }
}

} // namespace

TEST_CASE("workspace: install marker and writability") {
    TempDir t("marker");
    CHECK_FALSE(is_installed_layout(t.path));
    write_text(t.path / kInstallMarker, "{}");
    CHECK(is_installed_layout(t.path));
    CHECK(directory_writable(t.path));
    CHECK_FALSE(directory_writable(t.path / "missing"));
    CHECK_FALSE(fs::exists(t.path / ".aether-write-probe")); // the probe is cleaned up
}

TEST_CASE("workspace: documents dir honours the override") {
    TempDir t("docs");
    set_documents(t.path);
    CHECK(documents_dir() == t.path);
    CHECK(projects_dir() == t.path / "Aether Projects");
    clear_documents();
    CHECK_FALSE(documents_dir().empty());
}

TEST_CASE("workspace: starter project is created once and never overwritten") {
    TempDir install("install");
    TempDir docs("projects");
    make_install(install.path, true);

    auto first = ensure_starter_project(docs.path, install.path / "content", "Starter Project", "scenes/showcase.aescene");
    REQUIRE_MESSAGE(first.has_value(), first.error().message);
    CHECK(first->created);
    const fs::path dir = docs.path / "Starter Project";
    CHECK(first->manifest == dir / "Starter Project.aeproject");
    CHECK(fs::exists(dir / "content/scenes/showcase.aescene"));
    CHECK(fs::exists(dir / "content/scripts/a.lua"));

    auto p = load_project(first->manifest);
    REQUIRE_MESSAGE(p.has_value(), p.error().message);
    CHECK(p->name == "Starter Project");
    CHECK(p->content_root == (dir / "content").lexically_normal());
    CHECK(p->cooked_root == (dir / "assets").lexically_normal());
    CHECK_FALSE(p->cooked_assets);
    CHECK(p->startup_scene == fs::path("scenes/showcase.aescene"));

    // The user edits their project; a second run (e.g. after an upgrade) keeps it.
    write_text(dir / "content/scripts/a.lua", "-- my edit\n");
    write_text(dir / "content/scenes/mine.aescene", "{}");
    auto second = ensure_starter_project(docs.path, install.path / "content", "Starter Project", "scenes/showcase.aescene");
    REQUIRE(second.has_value());
    CHECK_FALSE(second->created);
    CHECK(read_text(dir / "content/scripts/a.lua") == "-- my edit\n");
    CHECK(fs::exists(dir / "content/scenes/mine.aescene"));

    // Missing template content is an error, not a half-made project.
    CHECK(ensure_starter_project(docs.path, install.path / "nope", "Other", "s.aescene").error().code == ErrorCode::NotFound);
    CHECK_FALSE(fs::exists(docs.path / "Other/Other.aeproject"));
}

TEST_CASE("workspace: installed layouts use a Documents project, development trees work in place") {
    TempDir docs("udocs");
    set_documents(docs.path);

    TempDir dev("dev");
    make_install(dev.path, false); // a source checkout: no marker, writable content
    auto d = default_user_project(dev.path);
    REQUIRE(d.has_value());
    CHECK(d->empty());

    TempDir inst("inst");
    make_install(inst.path, true);
    auto u = default_user_project(inst.path);
    REQUIRE_MESSAGE(u.has_value(), u.error().message);
    CHECK(*u == docs.path / "Aether Projects" / "Starter Project" / "Starter Project.aeproject");
    CHECK(fs::exists(*u));
    auto again = default_user_project(inst.path); // idempotent
    REQUIRE(again.has_value());
    CHECK(*again == *u);
    clear_documents();
}
