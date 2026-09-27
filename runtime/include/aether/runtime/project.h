// aether/runtime/project.h — the game project manifest (.aeproject) and packaging.
//
// A project manifest tells the runtime player what to run:
//   {
//     "format": "aether.project", "version": 1,
//     "name": "Showcase",
//     "startup_scene": "scenes/showcase.aescene",   // content-relative
//     "content_root": "content",                    // relative to the manifest's directory
//     "cooked_root": "assets",                      // idem
//     "cooked_assets": true,                        // load cooked data only (shipping)
//     "input_map": "input/game.json",               // optional, content-relative (InputMap JSON)
//     "window": { "title": "Showcase", "width": 1280, "height": 720,
//                 "resizable": true, "vsync": true, "fullscreen": false },
//     "simulation": { "fixed_delta": 0.0166667, "max_fixed_steps": 8 },
//     "rendering": { "exposure": 1.0, "sky": true },
//     "camera_controller": true                     // drive Fly/Orbit camera components
//   }
// Every key except "format" is optional. Unknown keys are ignored (warned) so newer
// manifests still load.
//
// package_project() turns a development tree into a self-contained, relocatable build:
//   <out>/<player executable>, <out>/<name>.aeproject, <out>/shaders/**,
//   <out>/content/** (scenes, scripts, input maps: the files loaded at runtime from source),
//   <out>/assets/** (every importable source cooked + asset_db.json).
// The packaged player finds its engine root (the directory holding shaders/) next to itself.
//
// Thread-affinity: any thread, one call at a time (package_project uses the JobSystem when
// it is initialised).
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"
#include "aether/core/window.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::gameplay {
struct AppDesc;
}

namespace aether::runtime {

inline constexpr const char* kProjectExtension = ".aeproject";

struct ProjectDesc {
    std::string           name = "Aether Game";
    std::filesystem::path startup_scene;  // content-relative (or absolute)
    std::filesystem::path content_root;   // absolute after load (empty => paths::content_dir())
    std::filesystem::path cooked_root;    // absolute after load (empty => paths::asset_dir())
    bool                  cooked_assets = false;
    std::filesystem::path input_map;      // content-relative; empty => built-in defaults
    WindowDesc            window{};
    bool                  fullscreen = false;
    f32                   fixed_delta = 1.0f / 60.0f;
    u32                   max_fixed_steps = 8;
    f32                   exposure = 1.0f;
    bool                  sky = true;
    bool                  camera_controller = true;
};

// Parses manifest text. Relative roots resolve against `base_dir` (the manifest's directory).
[[nodiscard]] Result<ProjectDesc> parse_project(StringView json, const std::filesystem::path& base_dir);
[[nodiscard]] Result<ProjectDesc> load_project(const std::filesystem::path& file);
// Serialises `project`; roots are written relative to `base_dir` when they lie below it.
[[nodiscard]] std::string project_to_json(const ProjectDesc& project, const std::filesystem::path& base_dir);

// The manifest the player runs when none is given: the single *.aeproject next to the
// executable, else in the engine root (game.aeproject preferred when there are several).
// Empty when there is none.
[[nodiscard]] std::filesystem::path find_default_project(const std::filesystem::path& executable_dir,
                                                         const std::filesystem::path& engine_root);

// Application settings for `project` (window, simulation, content/cooked roots, startup scene).
void apply_project(const ProjectDesc& project, gameplay::AppDesc& desc);

// ---- packaging ----------------------------------------------------------------------------------

struct PackageOptions {
    std::filesystem::path project_file;  // the development manifest
    std::filesystem::path output_dir;    // created; existing files are overwritten
    std::filesystem::path executable;    // copied into output_dir (empty: skip)
    std::filesystem::path shader_dir;    // copied to output_dir/shaders (empty: skip)
    bool                  force_cook = false;
};

struct PackageReport {
    u32              sources_cooked = 0;
    u32              sources_failed = 0;
    usize            assets = 0;         // records in the packaged asset database
    u32              files_copied = 0;   // runtime content + shaders + executable
    std::filesystem::path manifest;      // the packaged manifest
    std::vector<std::string> errors;     // cook failures (packaging continues past them)
};

// True for content files the runtime reads directly from disk (scenes, scripts, JSON data):
// these are copied into a package; everything else is either cooked or left out.
[[nodiscard]] bool is_runtime_content_file(const std::filesystem::path& file);

[[nodiscard]] Result<PackageReport> package_project(const PackageOptions& options);

} // namespace aether::runtime
