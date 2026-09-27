// aether-player — the Aether runtime player (Layer 5).
//
//   aether-player [project.aeproject] [--project <file>] [--check] [--cooked | --source-assets]
//                 [--package <out_dir> [--force-cook]] [shared app flags: --frames N, --scene S,
//                 --no-validation, --no-vsync, --width W, --height H, --title T]
//
// Without a project argument the player runs the *.aeproject next to its executable, else the
// one in the engine root (the repository's game.aeproject during development).
//   --check          verify the run on exit (see player.h); exit code 1 on failure
//   --cooked         load cooked assets only (AssetLoadMode::Runtime), as a shipped build does
//   --source-assets  import/cook from the content sources on demand (development)
//   --package <dir>  cook the project's content and write a self-contained, relocatable build
//                    (player + shaders + content + cooked assets + game.aeproject), then exit
//   --force-cook     with --package: re-import every source
// Exit codes: 0 success, 1 failure (initialisation, check or packaging), 2 usage error.
#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/gameplay/application.h"
#include "aether/runtime/player.h"
#include "aether/runtime/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace aether;

namespace {

struct PlayerArgs {
    fs::path            project;
    fs::path            package_dir;
    bool                check      = false;
    bool                force_cook = false;
    std::optional<bool> cooked; // --cooked / --source-assets override the manifest
    bool                usage_error = false;
};

// Flags of parse_command_line() that take a value (skipped when looking for the project).
bool is_shared_value_flag(const char* a) {
    for (const char* f : { "--frames", "--scene", "--width", "--height", "--title" }) {
        if (std::strcmp(a, f) == 0) return true;
    }
    return false;
}

PlayerArgs parse_player_args(int argc, char** argv) {
    PlayerArgs a;
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i];
        if (std::strcmp(s, "--project") == 0 || std::strcmp(s, "--package") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "aether-player: %s needs a path\n", s);
                a.usage_error = true;
                break;
            }
            (std::strcmp(s, "--project") == 0 ? a.project : a.package_dir) = argv[++i];
        } else if (std::strcmp(s, "--check") == 0) {
            a.check = true;
        } else if (std::strcmp(s, "--force-cook") == 0) {
            a.force_cook = true;
        } else if (std::strcmp(s, "--cooked") == 0) {
            a.cooked = true;
        } else if (std::strcmp(s, "--source-assets") == 0) {
            a.cooked = false;
        } else if (is_shared_value_flag(s)) {
            ++i;
        } else if (s[0] != '-' && a.project.empty()) {
            a.project = s;
        }
    }
    return a;
}

int package(const PlayerArgs& args, const fs::path& project_file, const char* argv0) {
    runtime::PackageOptions o;
    o.project_file = project_file;
    o.output_dir   = args.package_dir;
    o.executable   = paths::executable_dir() / fs::path(argv0).filename();
    o.shader_dir   = paths::shader_dir();
    o.force_cook   = args.force_cook;

    JobSystem::initialize();
    auto r = runtime::package_project(o);
    JobSystem::shutdown();
    if (!r) {
        std::fprintf(stderr, "aether-player: packaging failed: %s\n", r.error().message.c_str());
        return 1;
    }
    for (const std::string& e : r->errors) {
        std::fprintf(stderr, "  FAILED  %s\n", e.c_str());
    }
    std::printf("aether-player: packaged %s -> %s\n  %u sources cooked (%u failed), %zu assets, %u files copied\n",
                project_file.generic_string().c_str(), r->manifest.generic_string().c_str(), r->sources_cooked,
                r->sources_failed, r->assets, r->files_copied);
    return r->sources_failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const PlayerArgs args = parse_player_args(argc, argv);
    if (args.usage_error) {
        return 2;
    }

    fs::path project_file = args.project;
    if (project_file.empty()) {
        project_file = runtime::find_default_project(paths::executable_dir(), paths::engine_root());
    }
    if (project_file.empty()) {
        std::fprintf(stderr, "aether-player: no project given and no *%s found next to the player or in %s\n",
                     runtime::kProjectExtension, paths::engine_root().generic_string().c_str());
        return 2;
    }
    if (!args.package_dir.empty()) {
        return package(args, project_file, argv[0]);
    }

    auto project = runtime::load_project(project_file);
    if (!project) {
        AE_LOG_ERROR("Player", "{}", project.error().message);
        return 1;
    }
    if (args.cooked) {
        project->cooked_assets = *args.cooked;
    }

    gameplay::AppDesc defaults;
    defaults.imgui             = false;
    defaults.enable_validation = false; // shipping default; debug builds opt in below
#if !defined(NDEBUG)
    defaults.enable_validation = true;
#endif
    runtime::apply_project(*project, defaults);
    const gameplay::AppDesc desc = gameplay::parse_command_line(argc, argv, defaults);
    AE_LOG_INFO("Player", "project {} ({} assets)", project_file.generic_string(),
                desc.cooked_assets_only ? "cooked" : "source");

    runtime::PlayerOptions options;
    options.project = std::move(*project);
    options.check   = args.check;
    runtime::PlayerApp app(desc, std::move(options));
    if (auto init = app.initialize(); !init) {
        AE_LOG_ERROR("Player", "initialization failed: {}", init.error().message);
        return 1;
    }
    const int code = app.run();
    return code != 0 ? code : (app.check_passed() ? 0 : 1);
}
