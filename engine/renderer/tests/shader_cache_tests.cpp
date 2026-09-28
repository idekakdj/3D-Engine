// shader_cache_tests.cpp — the ADR-0014 SPIR-V cache of the PipelineLibrary: user-cache fill and
// hits, invalidation by any shader edit, the shipped (baked) spirv/ folder, corrupt entries and
// the off switch. Runs on a private copy of shaders/ so edits never touch the source tree.
#include "mock_device.h"
#include "pipelines.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace aether;
using namespace aether::renderer;
namespace fs = std::filesystem;

namespace {
struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / ("aether_shader_cache_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        fs::copy(fs::path(AE_RENDERER_SHADER_SOURCE_DIR), dir / "shaders", fs::copy_options::recursive);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    [[nodiscard]] fs::path root() const { return dir / "shaders"; }
    [[nodiscard]] fs::path user() const { return dir / "user"; }
};

struct Run {
    PipelineLibrary::CacheStats stats;
    u32                         failures = 0;
};

Run build(const Scratch& s, bool enabled = true) {
    test::MockDevice dev;
    PipelineLibrary  lib(dev, s.root());
    lib.set_user_cache_dir(s.user());
    lib.set_cache_enabled(enabled);
    Run r;
    r.failures = lib.build(build_pipeline_specs(dev.features()));
    r.stats = lib.cache_stats();
    lib.shutdown();
    return r;
}

u32 file_count(const fs::path& dir) {
    u32             n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        n += e.is_regular_file() ? 1u : 0u;
    }
    return n;
}
} // namespace

TEST_CASE("shader cache: compile once, then load from the user cache") {
    Scratch          s;
    test::MockDevice probe;
    const u32 shaders = static_cast<u32>(unique_shader_sources(build_pipeline_specs(probe.features())).size());
    REQUIRE(shaders > 20);

    const Run first = build(s);
    CHECK(first.failures == 0);
    CHECK(first.stats.compiled == shaders);
    CHECK(first.stats.cached == 0);
    CHECK(file_count(s.user()) == shaders); // no .tmp leftovers

    const Run second = build(s);
    CHECK(second.failures == 0);
    CHECK(second.stats.compiled == 0);
    CHECK(second.stats.cached == shaders);

    SUBCASE("any shader edit (even an include) invalidates every entry") {
        std::ofstream(s.root() / "renderer" / "brdf.glsl", std::ios::app) << "\n// edited\n";
        const Run edited = build(s);
        CHECK(edited.failures == 0);
        CHECK(edited.stats.compiled == shaders);
        CHECK(edited.stats.cached == 0);
    }
    SUBCASE("a baked spirv/ folder in the shader root is used first and not hashed") {
        fs::copy(s.user(), s.root() / kShippedSpirvDir);
        const Run shipped = build(s);
        CHECK(shipped.failures == 0);
        CHECK(shipped.stats.shipped == shaders);
        CHECK(shipped.stats.cached == 0);
        CHECK(shipped.stats.compiled == 0);
    }
    SUBCASE("corrupt entries are recompiled and rewritten") {
        for (const auto& e : fs::directory_iterator(s.user())) {
            std::ofstream(e.path(), std::ios::binary | std::ios::trunc) << "garbage!";
        }
        const Run repaired = build(s);
        CHECK(repaired.failures == 0);
        CHECK(repaired.stats.compiled == shaders);
        CHECK(build(s).stats.cached == shaders);
    }
    SUBCASE("the cache can be switched off") {
        const Run off = build(s, false);
        CHECK(off.failures == 0);
        CHECK(off.stats.compiled == shaders);
        CHECK(off.stats.cached == 0);
    }
}

TEST_CASE("shader cache: names depend on everything that changes the SPIR-V") {
    const fs::path     root(AE_RENDERER_SHADER_SOURCE_DIR);
    const ShaderSource a{ "renderer/depth.frag", rhi::ShaderStage::Fragment, {} };
    ShaderSource       b = a;
    b.defines.push_back({ "AE_ALPHA_MASK", "1" });
    const auto name = [&](const ShaderSource& src, u64 tree, bool dbg) {
        rhi::ShaderCompileOptions o = shader_compile_options(src, root);
        o.debug_info = dbg;
        return spirv_cache_name(tree, src, o);
    };
    CHECK(name(a, 1, false) == name(a, 1, false));
    CHECK(name(a, 1, false).size() == 20); // 16 hex + ".spv"
    CHECK(name(a, 1, false) != name(b, 1, false));
    CHECK(name(a, 1, false) != name(a, 2, false));
    CHECK(name(a, 1, false) != name(a, 1, true));
    // The absolute include path is not part of the name (build tree vs install folder).
    CHECK(spirv_cache_name(1, a, shader_compile_options(a, "/x")) == spirv_cache_name(1, a, shader_compile_options(a, "/y")));
    CHECK(shader_tree_hash(root) == shader_tree_hash(root));
    CHECK(read_spirv_file(root / "renderer" / "depth.frag").empty()); // not SPIR-V
}
