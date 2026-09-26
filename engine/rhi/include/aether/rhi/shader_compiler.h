// aether/rhi/shader_compiler.h — device-free GLSL -> SPIR-V compilation (glslang).
//
// FROZEN CONTRACT (ADR-0002). Usable without a Device, so tools, tests and an offline
// cook step can validate/compile shaders. Device::compile_glsl*() forward to these.
// Targets Vulkan 1.3 / SPIR-V 1.6. #include resolution order: directory of the including
// file, then options.include_dirs, then paths::shader_dir(). Defines are injected as a
// preamble after #version. Errors carry "<file>:<line>: <message>" text. Thread-safe.
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"
#include "aether/rhi/enums.h"
#include "aether/rhi/resources.h"

#include <filesystem>
#include <vector>

namespace aether::rhi {

Result<std::vector<u32>> compile_glsl(ShaderStage stage, StringView source,
                                      StringView name = "inline",
                                      const ShaderCompileOptions& options = {});

Result<std::vector<u32>> compile_glsl_file(ShaderStage stage, const std::filesystem::path& file,
                                           const ShaderCompileOptions& options = {});

// Stage from extension: .vert .frag .comp .geom .task .mesh (also *.vert.glsl etc.).
Result<ShaderStage> shader_stage_from_path(const std::filesystem::path& file);

} // namespace aether::rhi
