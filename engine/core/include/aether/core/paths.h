// aether/core/paths.h — exe-relative content root discovery.
//
// FROZEN CONTRACT (ADR-0001). Never use CWD-relative paths. All content lookups go
// through here so the app behaves identically launched from an IDE, Explorer, or CI.
#pragma once

#include "aether/core/types.h"

#include <filesystem>
#include <vector>

namespace aether::paths {

// Absolute path to the running executable's directory.
const std::filesystem::path& executable_dir();

// The engine/project root: the nearest ancestor of executable_dir() that contains a
// `shaders/` directory (falls back to executable_dir()). Resolved once, cached.
const std::filesystem::path& engine_root();

std::filesystem::path shader_dir(); // engine_root()/shaders
std::filesystem::path asset_dir();  // engine_root()/assets   (cooked)
std::filesystem::path content_dir(); // engine_root()/content (source)

// Read an entire file into bytes. Returns empty on failure (logs a warning).
std::vector<byte> read_file(const std::filesystem::path& p);
String            read_text_file(const std::filesystem::path& p);

} // namespace aether::paths
