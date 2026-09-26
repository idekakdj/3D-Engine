// aether-editor — entry point.
//
// Usage: aether-editor [--scene <file.aescene>] [--select <entity name>] [--self-test] [shared app flags]
//   --self-test  run the scripted editor workflow (self_test.cpp) and exit with its result.
#include "aether/core/log.h"
#include "editor_app.h"

#include <cstring>

int main(int argc, char** argv) {
    using namespace aether;
    gameplay::AppDesc defaults;
    defaults.window.title  = "Aether Editor";
    defaults.window.width  = 1600;
    defaults.window.height = 900;
    const gameplay::AppDesc desc = gameplay::parse_command_line(argc, argv, defaults);

    editor::EditorOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            options.self_test = true;
        } else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            options.select = argv[++i];
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
