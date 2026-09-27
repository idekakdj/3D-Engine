// aether/runtime/workspace.h — where an INSTALLED Aether keeps user data (ADR-0013).
//
// An installed Aether (Program Files / AppData\Local\Programs, or an unpacked zip) must never write
// into its own folder: the editor imports sources into `<project>/assets` and saves scenes into
// `<project>/content`. So when the engine root is an installed layout (it holds the install marker
// written by `cmake --install`, or its content folder is read-only) the editor and the player work
// on a user project under Documents instead:
//
//   <Documents>/Aether Projects/Starter Project/
//       Starter Project.aeproject   (content_root "content", cooked_root "assets")
//       content/                    (copied once from the installed starter content)
//       assets/                     (cooked data, created on first run)
//
// The starter project is created on first use and never overwritten afterwards (user edits are
// kept across upgrades). A development checkout (source tree, no marker) is unaffected.
// Thread-affinity: any thread, one call at a time.
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"

#include <filesystem>
#include <string_view>

namespace aether::runtime {

// Written to the install root by the CMake install rules.
inline constexpr const char* kInstallMarker   = "aether-install.json";
inline constexpr const char* kProjectsFolder  = "Aether Projects";
inline constexpr const char* kStarterProject  = "Starter Project";
inline constexpr const char* kStarterScene    = "scenes/showcase.aescene";

// True when `engine_root` holds the install marker.
[[nodiscard]] bool is_installed_layout(const std::filesystem::path& engine_root);

// True when a file can be created in `dir` (probe file created and removed).
[[nodiscard]] bool directory_writable(const std::filesystem::path& dir);

// The user's Documents folder: env AETHER_DOCUMENTS_DIR (tests / portable use), else Windows'
// Documents known folder (follows OneDrive redirection), else $XDG_DOCUMENTS_DIR, else ~/Documents,
// else the temp directory.
[[nodiscard]] std::filesystem::path documents_dir();

// <documents_dir()>/Aether Projects
[[nodiscard]] std::filesystem::path projects_dir();

struct StarterProject {
    std::filesystem::path manifest;
    bool                  created = false; // false: it already existed and was left untouched
};

// Creates `<projects_root>/<name>/` from `template_content` (copied recursively) plus
// `<name>.aeproject`, unless that manifest already exists. Never overwrites existing files.
[[nodiscard]] Result<StarterProject> ensure_starter_project(const std::filesystem::path& projects_root,
                                                            const std::filesystem::path& template_content,
                                                            std::string_view name,
                                                            const std::filesystem::path& startup_scene);

// The project an installed editor / player opens when none is given: the starter project under
// projects_dir(), created from `<engine_root>/content` on first use. Empty path (no error) when
// `engine_root` is not an installed layout and its content folder is writable (development).
[[nodiscard]] Result<std::filesystem::path> default_user_project(const std::filesystem::path& engine_root);

} // namespace aether::runtime
