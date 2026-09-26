// aether/core/paths_ext.h — per-user writable locations (additive to paths.h).
//
// ADDITIVE (not a frozen contract). The repo may live in a synced folder (OneDrive), so
// generated/cached data never goes next to the sources.
#pragma once

#include "aether/core/types.h"

#include <filesystem>

namespace aether::paths {

// UTF-8 text of a path, never throws (std::filesystem::path::string() can on Windows).
String to_utf8(const std::filesystem::path& p);

// Per-user cache root, created on first use:
//   env AETHER_CACHE_DIR, else %USERPROFILE%/.aether/cache (Windows) /
//   $HOME/.aether/cache, else <temp>/aether-cache. Thread-safe; resolved once.
const std::filesystem::path& cache_dir();

// How engine_root() was resolved (for diagnostics / logging).
enum class RootSource { Environment, ExecutableAncestor, SourceTree, ExecutableDir };
RootSource engine_root_source();

} // namespace aether::paths
