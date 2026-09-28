// aether-editor — entry point.
//
// Usage: aether-editor [--project <file.aeproject>] [--scene <file.aescene>] [--select <entity name>]
//                      [--browse <content dir>] [--self-test] [shared app flags]
//   --project    open a project (content + cooked folders, startup scene) instead of the engine's
//                content folder. An INSTALLED editor without --project opens (creating it on first
//                run) "Documents/Aether Projects/Starter Project" (ADR-0013).
//   --browse     open the asset browser in this content-relative folder (e.g. samples/props)
//   --projects   open the Projects window (open / create projects) at startup
//   --self-test  run the scripted editor workflow (self_test.cpp) and exit with its result.
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/runtime/project.h"
#include "aether/runtime/workspace.h"
#include "editor_app.h"

#include <cstring>
#include <filesystem>

int main(int argc, char** argv) {
    using namespace aether;
    gameplay::AppDesc defaults;
    defaults.window.title  = "Aether Editor";
    defaults.window.width  = 1600;
    defaults.window.height = 900;
    defaults.window_icon   = "resources/aether.png"; // ADR-0014
    defaults.splash_image  = "resources/splash.png";
    defaults.splash_title  = "Aether Editor";
#ifdef NDEBUG
    defaults.enable_validation = false; // release / installed builds: no Vulkan SDK layers needed
#endif

    // ---- project: --project, else the installed default (Documents), else the engine content ----
    std::filesystem::path project_file;
    std::string           project_name;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0) {
            project_file = argv[i + 1];
        }
    }
    if (project_file.empty()) {
        auto def = runtime::default_user_project(paths::engine_root());
        if (!def) {
            AE_LOG_ERROR("Editor", "cannot prepare the user project: {}", def.error().message);
            return 1;
        }
        project_file = *def;
    }
    if (!project_file.empty()) {
        auto project = runtime::load_project(project_file);
        if (!project) {
            AE_LOG_ERROR("Editor", "{}", project.error().message);
            return 1;
        }
        defaults.content_root  = project->content_root;
        defaults.cooked_root   = project->cooked_root;
        defaults.startup_scene = project->startup_scene;
        defaults.window.title  = "Aether Editor - " + project->name;
        project_name           = project->name;
        AE_LOG_INFO("Editor", "project '{}' ({})", project->name, project_file.generic_string());
    }
    const gameplay::AppDesc desc = gameplay::parse_command_line(argc, argv, defaults);

    editor::EditorOptions options;
    if (!project_file.empty()) {
        std::error_code ec;
        options.project_file = std::filesystem::weakly_canonical(std::filesystem::absolute(project_file, ec), ec);
        options.project_name = project_name;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            options.self_test = true;
        } else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            options.select = argv[++i];
        } else if (std::strcmp(argv[i], "--browse") == 0 && i + 1 < argc) {
            options.browse = argv[++i];
        } else if (std::strcmp(argv[i], "--projects") == 0) {
            options.show_projects = true;
        }
    }

    editor::EditorApp app(desc, options);
    if (auto init = app.initialize(); !init) {
        AE_LOG_ERROR("Editor", "initialization failed: {}", init.error().message);
        return 1;
    }
    const int code = app.run();
    if (options.self_test && !app.self_test_passed()) {
        return 1;
    }
    return code;
}
