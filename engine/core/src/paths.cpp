// paths.cpp — executable-relative content root discovery + whole-file reads.
//
// engine_root() resolution order (first hit wins, resolved once, thread-safe):
//   1. env AETHER_ROOT                                  (explicit override; must exist)
//   2. nearest ancestor of executable_dir() with shaders/ (shipped layout)
//   3. compiled-in AE_SOURCE_DIR (if it has shaders/)    (dev: build trees live outside
//                                                         the repo, see ADR-0002)
//   4. executable_dir()                                  (last resort)
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/core/paths_ext.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace aether::paths {
namespace {

// Reads an environment variable as a path (wide API on Windows: non-ASCII safe).
fs::path env_path(const char* name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD  needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD  written = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return fs::path(value);
#else
    const char* v = std::getenv(name);
    return (v && *v) ? fs::path(v) : fs::path{};
#endif
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

bool has_shaders_dir(const fs::path& dir) {
    return is_dir(dir / "shaders");
}

fs::path compute_executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            break;
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2); // truncated: grow and retry
    }
#else
    std::error_code ec;
    const fs::path  exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe.parent_path();
    }
#endif
    std::error_code ec2;
    return fs::current_path(ec2); // degenerate fallback; logged by the caller's use
}

struct RootInfo {
    fs::path   path;
    RootSource source = RootSource::ExecutableDir;
};

RootInfo compute_root() {
    if (fs::path env = env_path("AETHER_ROOT"); !env.empty()) {
        if (is_dir(env)) {
            return { env.lexically_normal(), RootSource::Environment };
        }
        AE_LOG_WARN("Paths", "AETHER_ROOT='{}' is not a directory; ignoring", to_utf8(env));
    }

    const fs::path& exe = executable_dir();
    for (fs::path dir = exe; !dir.empty(); dir = dir.parent_path()) {
        if (has_shaders_dir(dir)) {
            return { dir, RootSource::ExecutableAncestor };
        }
        if (dir == dir.parent_path()) {
            break; // reached the filesystem root
        }
    }

#ifdef AE_SOURCE_DIR
    const fs::path source(std::u8string_view(reinterpret_cast<const char8_t*>(AE_SOURCE_DIR)));
    if (has_shaders_dir(source)) {
        return { source.lexically_normal().make_preferred(), RootSource::SourceTree };
    }
#endif

    return { exe, RootSource::ExecutableDir };
}

const RootInfo& root_info() {
    static const RootInfo info = [] {
        RootInfo    r = compute_root();
        const char* how = "executable dir (no shaders/ found!)";
        switch (r.source) {
        case RootSource::Environment: how = "AETHER_ROOT"; break;
        case RootSource::ExecutableAncestor: how = "executable ancestor"; break;
        case RootSource::SourceTree: how = "source tree (AE_SOURCE_DIR)"; break;
        case RootSource::ExecutableDir: break;
        }
        AE_LOG_INFO("Paths", "engine root: {} [{}]", to_utf8(r.path), how);
        return r;
    }();
    return info;
}

} // namespace

String to_utf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return String(reinterpret_cast<const char*>(u8.data()), u8.size());
}

const fs::path& executable_dir() {
    static const fs::path dir = compute_executable_dir();
    return dir;
}

const fs::path& engine_root() {
    return root_info().path;
}

RootSource engine_root_source() {
    return root_info().source;
}

fs::path shader_dir() {
    return engine_root() / "shaders";
}

fs::path asset_dir() {
    return engine_root() / "assets";
}

fs::path content_dir() {
    return engine_root() / "content";
}

const fs::path& cache_dir() {
    static const fs::path dir = [] {
        fs::path p = env_path("AETHER_CACHE_DIR");
        if (p.empty()) {
#ifdef _WIN32
            const fs::path home = env_path("USERPROFILE");
#else
            const fs::path home = env_path("HOME");
#endif
            if (!home.empty()) {
                p = home / ".aether" / "cache";
            }
        }
        if (p.empty()) {
            std::error_code ec;
            p = fs::temp_directory_path(ec) / "aether-cache";
        }
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec) {
            AE_LOG_WARN("Paths", "cannot create cache dir '{}': {}", to_utf8(p), ec.message());
        }
        return p;
    }();
    return dir;
}

std::vector<byte> read_file(const fs::path& p) {
    std::ifstream file(p, std::ios::binary | std::ios::ate);
    if (!file) {
        AE_LOG_WARN("Paths", "read_file: cannot open '{}'", to_utf8(p));
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        AE_LOG_WARN("Paths", "read_file: cannot size '{}'", to_utf8(p));
        return {};
    }
    std::vector<byte> data(static_cast<usize>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        AE_LOG_WARN("Paths", "read_file: short read on '{}'", to_utf8(p));
        return {};
    }
    return data;
}

String read_text_file(const fs::path& p) {
    std::vector<byte> bytes = read_file(p);
    usize             start = 0;
    // Strip a UTF-8 BOM (glslang and most parsers reject it).
    if (bytes.size() >= 3 && bytes[0] == byte{ 0xEF } && bytes[1] == byte{ 0xBB } && bytes[2] == byte{ 0xBF }) {
        start = 3;
    }
    if (bytes.size() <= start) {
        return {};
    }
    return String(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
}

} // namespace aether::paths
