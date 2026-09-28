// aether-shaderc — bakes the renderer's shader cache at build time (ADR-0014).
//
//   aether-shaderc <shader_root> <out_dir>
//
// Compiles every shader permutation of build_pipeline_specs() (mesh-shader variants included) with
// exactly the options the runtime PipelineLibrary uses and writes <out_dir>/<spirv_cache_name()>.
// The build installs <out_dir> as <install>/shaders/spirv, so an installed engine starts without
// compiling GLSL. Built in the same configuration as the apps, so the debug-info flag matches.
#include "pipelines.h"

#include "aether/rhi/shader_compiler.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

using namespace aether;
using namespace aether::renderer;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: aether-shaderc <shader_root> <out_dir>\n");
        return 2;
    }
    const fs::path root = argv[1];
    const fs::path out = argv[2];
    std::error_code ec;
    if (!fs::exists(root / "renderer", ec)) {
        std::fprintf(stderr, "aether-shaderc: '%s' has no renderer/ folder\n", root.string().c_str());
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();

    // Superset of every device: the meshlet (mesh-shader) permutations are included; the other
    // feature bits only change pipeline state, never shader code.
    rhi::DeviceFeatures features{};
    features.mesh_shaders = true;
    features.depth_clamp = true;
    const std::vector<ShaderSource> sources = unique_shader_sources(build_pipeline_specs(features));
    const u64                       tree = shader_tree_hash(root);

    fs::remove_all(out, ec); // stale modules of older shader trees would only waste space
    fs::create_directories(out, ec);
    if (ec) {
        std::fprintf(stderr, "aether-shaderc: cannot create '%s': %s\n", out.string().c_str(), ec.message().c_str());
        return 1;
    }
    u32 failed = 0;
    for (const ShaderSource& src : sources) {
        const rhi::ShaderCompileOptions opts = shader_compile_options(src, root);
        auto spirv = rhi::compile_glsl_file(src.stage, root / src.file, opts);
        if (!spirv) {
            std::fprintf(stderr, "aether-shaderc: %s\n%s\n", src.key().c_str(), spirv.error().message.c_str());
            ++failed;
            continue;
        }
        if (!write_spirv_file(out / spirv_cache_name(tree, src, opts), spirv.value())) {
            std::fprintf(stderr, "aether-shaderc: cannot write into '%s'\n", out.string().c_str());
            return 1;
        }
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("aether-shaderc: %zu shaders -> %s (%.0f ms, %u failed)\n", sources.size() - failed,
                out.string().c_str(), ms, failed);
    return failed == 0 ? 0 : 1;
}
