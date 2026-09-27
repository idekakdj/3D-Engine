// shader_tests.cpp — compiles EVERY shader in shaders/renderer/ with the device-free
// glslang wrapper (default defines), then every permutation the renderer actually builds
// (build_pipeline_specs), failing on any error.
#include "pipelines.h"

#include "aether/rhi/shader_compiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>

using namespace aether;
using namespace aether::renderer;

namespace {
std::filesystem::path shader_root() { return std::filesystem::path(AE_RENDERER_SHADER_SOURCE_DIR); }

rhi::ShaderCompileOptions options(const std::vector<rhi::ShaderDefine>& extra) {
    rhi::ShaderCompileOptions o;
    o.defines = shared_shader_defines();
    o.defines.insert(o.defines.end(), extra.begin(), extra.end());
    o.include_dirs = { shader_root().string() };
    return o;
}
} // namespace

TEST_CASE("every shader in shaders/renderer compiles") {
    const std::filesystem::path dir = shader_root() / "renderer";
    REQUIRE(std::filesystem::exists(dir));
    u32 compiled = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string ext = entry.path().extension().string();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp") {
            continue;
        }
        auto stage = rhi::shader_stage_from_path(entry.path());
        REQUIRE(stage);
        auto spirv = rhi::compile_glsl_file(stage.value(), entry.path(), options({}));
        INFO(entry.path().filename().string());
        if (!spirv) {
            FAIL_CHECK(spirv.error().message);
            continue;
        }
        CHECK_FALSE(spirv.value().empty());
        ++compiled;
    }
    CHECK(compiled >= 20);
}

TEST_CASE("every renderer pipeline permutation compiles") {
    rhi::DeviceFeatures features;
    features.depth_clamp = true;
    const std::vector<PipelineSpec> specs = build_pipeline_specs(features);
    REQUIRE(specs.size() == kPipelineCount);
    std::set<std::string> seen;
    for (const PipelineSpec& spec : specs) {
        CHECK_FALSE(spec.name.empty());
        for (const ShaderSource* src : { &spec.vs, &spec.fs, &spec.cs }) {
            if (src->empty() || !seen.insert(src->key()).second) {
                continue;
            }
            auto spirv = rhi::compile_glsl_file(src->stage, shader_root() / src->file, options(src->defines));
            INFO(spec.name << " : " << src->key());
            if (!spirv) {
                FAIL_CHECK(spirv.error().message);
            }
        }
    }
    CHECK(seen.size() >= 20);
    // Mesh pipeline table is dense and unique.
    std::set<u32> idx;
    for (bool sk : { false, true }) {
        for (bool m : { false, true }) {
            for (bool ds : { false, true }) {
                idx.insert(mesh_pipeline_index(MeshPass::Depth, sk, m, ds));
                idx.insert(mesh_pipeline_index(MeshPass::Shadow, sk, m, ds));
                idx.insert(mesh_pipeline_index(MeshPass::Forward, sk, m, ds));
                idx.insert(mesh_pipeline_index(MeshPass::Translucent, sk, m, ds));
                idx.insert(mesh_pipeline_index(MeshPass::Overdraw, sk, m, ds));
                idx.insert(mesh_pipeline_index(MeshPass::Pick, sk, m, ds));
            }
        }
    }
    CHECK(idx.size() == kMeshPipelineCount);
    CHECK(*idx.rbegin() == kMeshPipelineCount - 1);
}
