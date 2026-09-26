// shader_compiler.cpp — device-free GLSL -> SPIR-V (glslang C API), Vulkan 1.3 / SPIR-V 1.6.
//
// * glslang_initialize_process() runs exactly once (std::call_once); compiles are
//   thread-safe (each call owns its glslang shader/program objects).
// * Defines + "#extension GL_GOOGLE_include_directive" are injected right after the
//   #version line, followed by a #line directive (user line numbers are kept).
// * #include resolution: directory of the including file (quoted includes only), then
//   options.include_dirs (relative ones are taken relative to paths::shader_dir()), then
//   paths::shader_dir(). Absolute include paths are used as-is.
// * Diagnostics are rewritten to "<file>:<line>: <message>", one per line.
#include "aether/rhi/shader_compiler.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/core/paths_ext.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cctype>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace aether::rhi {
namespace {

namespace fs = std::filesystem;

void ensure_glslang_process() {
    static std::once_flag once;
    std::call_once(once, [] {
        glslang_initialize_process();
        std::atexit([] { glslang_finalize_process(); });
    });
}

bool to_glslang_stage(ShaderStage s, glslang_stage_t& out) {
    switch (s) {
    case ShaderStage::Vertex: out = GLSLANG_STAGE_VERTEX; return true;
    case ShaderStage::Fragment: out = GLSLANG_STAGE_FRAGMENT; return true;
    case ShaderStage::Compute: out = GLSLANG_STAGE_COMPUTE; return true;
    case ShaderStage::Geometry: out = GLSLANG_STAGE_GEOMETRY; return true;
    case ShaderStage::Task: out = GLSLANG_STAGE_TASK; return true;
    case ShaderStage::Mesh: out = GLSLANG_STAGE_MESH; return true;
    default: return false;
    }
}

fs::path from_utf8(std::string_view s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// ---- #include callbacks ---------------------------------------------------------------

struct IncludeContext {
    fs::path              root_dir; // directory of the root file (empty for inline source)
    std::vector<fs::path> include_dirs;
};

struct IncludeResult {
    glsl_include_result_t result{}; // first member: freed via reinterpret_cast
    std::string           name;
    std::string           data;
};

glsl_include_result_t* make_result(std::string name, std::string data) {
    auto* r                 = new IncludeResult();
    r->name                 = std::move(name);
    r->data                 = std::move(data);
    r->result.header_name   = r->name.c_str(); // "" => failure, data holds the message
    r->result.header_data   = r->data.c_str();
    r->result.header_length = r->data.size();
    return &r->result;
}

glsl_include_result_t* resolve_include(void* ctx, const char* header, const char* includer, bool system) {
    const auto*     c = static_cast<const IncludeContext*>(ctx);
    const fs::path  h = from_utf8(header ? header : "");
    std::vector<fs::path> candidates;
    if (h.is_absolute()) {
        candidates.push_back(h);
    } else {
        if (!system) {
            const fs::path dir = (includer && *includer) ? from_utf8(includer).parent_path() : c->root_dir;
            if (!dir.empty()) {
                candidates.push_back(dir / h);
            }
        }
        for (const fs::path& d : c->include_dirs) {
            candidates.push_back(d / h);
        }
        candidates.push_back(paths::shader_dir() / h);
    }
    for (const fs::path& cand : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
            const fs::path resolved = cand.lexically_normal();
            return make_result(resolved.generic_string(), paths::read_text_file(resolved));
        }
    }
    std::string searched;
    for (const fs::path& cand : candidates) {
        searched += "\n    " + paths::to_utf8(cand.lexically_normal());
    }
    return make_result("", std::format("cannot find include \"{}\"; searched:{}", header ? header : "", searched));
}

glsl_include_result_t* include_local(void* ctx, const char* header, const char* includer, size_t /*depth*/) {
    return resolve_include(ctx, header, includer, false);
}

glsl_include_result_t* include_system(void* ctx, const char* header, const char* includer, size_t /*depth*/) {
    return resolve_include(ctx, header, includer, true);
}

int free_include(void* /*ctx*/, glsl_include_result_t* result) {
    delete reinterpret_cast<IncludeResult*>(result);
    return 0;
}

// ---- diagnostics ---------------------------------------------------------------------

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// "ERROR: 0:12: 'x' : undeclared identifier" -> "<root>:12: error: 'x' : undeclared identifier"
std::string format_log(std::string_view log, std::string_view root_name) {
    std::string out;
    usize       pos = 0;
    while (pos < log.size()) {
        usize end = log.find('\n', pos);
        if (end == std::string_view::npos) end = log.size();
        std::string_view line = log.substr(pos, end - pos);
        pos                   = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (trim(line).empty()) continue;

        std::string_view severity;
        for (std::string_view prefix : { std::string_view("ERROR: "), std::string_view("WARNING: ") }) {
            if (line.starts_with(prefix)) {
                severity = prefix.substr(0, prefix.size() - 2);
                line.remove_prefix(prefix.size());
                break;
            }
        }
        // Skip glslang's summary lines ("1 compilation errors.  No code generated.").
        if (line.find("compilation errors") != std::string_view::npos ||
            line.find("No code generated") != std::string_view::npos) {
            continue;
        }
        // Locate "<file>:<digits>:" - the first ':' followed by digits and another ':'.
        usize colon = line.find(':');
        bool  located = false;
        while (colon != std::string_view::npos) {
            usize d = colon + 1;
            while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
            if (d > colon + 1 && d < line.size() && line[d] == ':') {
                std::string_view file = line.substr(0, colon);
                std::string_view num  = line.substr(colon + 1, d - colon - 1);
                if (file.empty() || file == "0") file = root_name;
                const std::string sev = severity.empty() ? "" : (severity == "ERROR" ? "error: " : "warning: ");
                out += std::format("{}:{}: {}{}\n", file, num, sev, trim(line.substr(d + 1)));
                located = true;
                break;
            }
            colon = line.find(':', colon + 1);
        }
        if (!located) {
            out += std::format("{}: {}\n", root_name, trim(line));
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

struct ShaderDeleter {
    void operator()(glslang_shader_t* s) const { glslang_shader_delete(s); }
};
struct ProgramDeleter {
    void operator()(glslang_program_t* p) const { glslang_program_delete(p); }
};

// Inserts "#extension GL_GOOGLE_include_directive" + defines right after the #version
// line, then "#line" so diagnostics keep the user's line numbers. (glslang's preamble is
// unusable with the C API: the preprocessed text echoes it BEFORE #version, which the
// separate parse stage then rejects.)
std::string inject_prologue(const std::string& source, const ShaderCompileOptions& options) {
    std::string prologue = "#extension GL_GOOGLE_include_directive : require\n";
    for (const ShaderDefine& d : options.defines) {
        prologue += "#define " + d.name + (d.value.empty() ? "" : " " + d.value) + "\n";
    }
    usize line_start = 0;
    u32   line_no    = 1;
    while (line_start < source.size()) {
        usize line_end = source.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = source.size();
        }
        const std::string_view line  = std::string_view(source).substr(line_start, line_end - line_start);
        const usize            first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos && line.substr(first).starts_with("#version")) {
            std::string out = source.substr(0, line_end);
            out += '\n';
            out += prologue;
            out += std::format("#line {}\n", line_no + 1);
            if (line_end + 1 < source.size()) {
                out += source.substr(line_end + 1);
            }
            return out;
        }
        line_start = line_end + 1;
        ++line_no;
    }
    return "#version 460\n" + prologue + "#line 1\n" + source; // no #version: default 460
}

Result<std::vector<u32>> compile_impl(ShaderStage stage, const std::string& source, const std::string& name,
                                      const fs::path& root_dir, const ShaderCompileOptions& options) {
    glslang_stage_t gstage{};
    if (!to_glslang_stage(stage, gstage)) {
        return Error{ ErrorCode::InvalidArgument, name + ": shader stage must be exactly one stage" };
    }
    ensure_glslang_process();

    IncludeContext ctx;
    ctx.root_dir = root_dir;
    for (const std::string& dir : options.include_dirs) {
        fs::path p = from_utf8(dir);
        ctx.include_dirs.push_back(p.is_absolute() ? p : paths::shader_dir() / p);
    }

    const std::string code = inject_prologue(source, options);

    int messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    if (options.debug_info) {
        messages |= GLSLANG_MSG_DEBUG_INFO_BIT;
    }

    glslang_input_t input{};
    input.language                          = GLSLANG_SOURCE_GLSL;
    input.stage                             = gstage;
    input.client                            = GLSLANG_CLIENT_VULKAN;
    input.client_version                    = GLSLANG_TARGET_VULKAN_1_3;
    input.target_language                   = GLSLANG_TARGET_SPV;
    input.target_language_version           = GLSLANG_TARGET_SPV_1_6;
    input.code                              = code.c_str();
    input.default_version                   = 460;
    input.default_profile                   = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = 0;
    input.forward_compatible                = 0;
    input.messages                          = static_cast<glslang_messages_t>(messages);
    input.resource                          = glslang_default_resource();
    input.callbacks.include_local           = &include_local;
    input.callbacks.include_system          = &include_system;
    input.callbacks.free_include_result     = &free_include;
    input.callbacks_ctx                     = &ctx;

    std::unique_ptr<glslang_shader_t, ShaderDeleter> shader(glslang_shader_create(&input));
    if (!shader) {
        return Error{ ErrorCode::Internal, name + ": glslang_shader_create failed" };
    }
    if (!glslang_shader_preprocess(shader.get(), &input) || !glslang_shader_parse(shader.get(), &input)) {
        return Error{ ErrorCode::CompilationFailed, format_log(glslang_shader_get_info_log(shader.get()), name) };
    }

    std::unique_ptr<glslang_program_t, ProgramDeleter> program(glslang_program_create());
    glslang_program_add_shader(program.get(), shader.get());
    if (!glslang_program_link(program.get(), messages)) {
        return Error{ ErrorCode::CompilationFailed, format_log(glslang_program_get_info_log(program.get()), name) };
    }

    glslang_spv_options_t spv{};
    spv.generate_debug_info = options.debug_info;
    spv.disable_optimizer   = true;
    spv.validate            = false;
    glslang_program_SPIRV_generate_with_options(program.get(), gstage, &spv);

    const usize words = glslang_program_SPIRV_get_size(program.get());
    if (words == 0) {
        return Error{ ErrorCode::CompilationFailed, name + ": SPIR-V generation produced no code" };
    }
    std::vector<u32> spirv(words);
    glslang_program_SPIRV_get(program.get(), spirv.data());
    if (const char* msgs = glslang_program_SPIRV_get_messages(program.get()); msgs && *msgs) {
        AE_LOG_WARN("Shader", "{}: {}", name, trim(msgs));
    }
    return spirv;
}

} // namespace

Result<std::vector<u32>> compile_glsl(ShaderStage stage, StringView source, StringView name,
                                      const ShaderCompileOptions& options) {
    // Inline sources have no including-file directory; a path-like name supplies one.
    fs::path        root_dir;
    const fs::path  as_path = from_utf8(name);
    std::error_code ec;
    if (as_path.has_parent_path() && fs::is_directory(as_path.parent_path(), ec)) {
        root_dir = as_path.parent_path();
    }
    return compile_impl(stage, std::string(source), std::string(name), root_dir, options);
}

Result<std::vector<u32>> compile_glsl_file(ShaderStage stage, const fs::path& file, const ShaderCompileOptions& options) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return Error{ ErrorCode::NotFound, std::format("{}: shader file not found", paths::to_utf8(file)) };
    }
    const std::string source = paths::read_text_file(file);
    if (source.empty()) {
        return Error{ ErrorCode::IoError, std::format("{}: empty or unreadable", paths::to_utf8(file)) };
    }
    const fs::path normal = file.lexically_normal();
    return compile_impl(stage, source, normal.generic_string(), normal.parent_path(), options);
}

Result<ShaderStage> shader_stage_from_path(const fs::path& file) {
    fs::path p = file;
    if (p.extension() == ".glsl") {
        p = p.stem(); // name.vert.glsl -> name.vert
    }
    const std::string ext = p.extension().string();
    if (ext == ".vert") return ShaderStage::Vertex;
    if (ext == ".frag") return ShaderStage::Fragment;
    if (ext == ".comp") return ShaderStage::Compute;
    if (ext == ".geom") return ShaderStage::Geometry;
    if (ext == ".task") return ShaderStage::Task;
    if (ext == ".mesh") return ShaderStage::Mesh;
    return Error{ ErrorCode::InvalidArgument, std::format("{}: unknown shader stage extension", paths::to_utf8(file)) };
}

} // namespace aether::rhi
