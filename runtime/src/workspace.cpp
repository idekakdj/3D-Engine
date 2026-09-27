// workspace.cpp — see aether/runtime/workspace.h.
#include "aether/runtime/workspace.h"

#include "aether/core/log.h"
#include "aether/runtime/project.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <string>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace aether::runtime {
namespace {

fs::path env_path(const char* name) {
#ifdef _WIN32
    // _wgetenv keeps non-ASCII user names intact.
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const wchar_t* v = _wgetenv(wname.c_str());
    return v != nullptr && *v != 0 ? fs::path(v) : fs::path{};
#else
    const char* v = std::getenv(name);
    return v != nullptr && *v != 0 ? fs::path(v) : fs::path{};
#endif
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

} // namespace

bool is_installed_layout(const fs::path& engine_root) {
    std::error_code ec;
    return fs::is_regular_file(engine_root / kInstallMarker, ec);
}

bool directory_writable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    const fs::path probe = dir / ".aether-write-probe";
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

fs::path documents_dir() {
    if (fs::path p = env_path("AETHER_DOCUMENTS_DIR"); !p.empty()) {
        return p;
    }
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        fs::path p(raw);
        CoTaskMemFree(raw);
        return p;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    if (fs::path home = env_path("USERPROFILE"); !home.empty()) {
        return home / "Documents";
    }
#else
    if (fs::path xdg = env_path("XDG_DOCUMENTS_DIR"); !xdg.empty()) {
        return xdg;
    }
    if (fs::path home = env_path("HOME"); !home.empty()) {
        return home / "Documents";
    }
#endif
    std::error_code ec;
    return fs::temp_directory_path(ec);
}

fs::path projects_dir() { return documents_dir() / kProjectsFolder; }

Result<StarterProject> ensure_starter_project(const fs::path& projects_root, const fs::path& template_content,
                                              std::string_view name, const fs::path& startup_scene) {
    StarterProject  out;
    const fs::path  dir = projects_root / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    out.manifest        = dir / (dir.filename().native() + fs::path(kProjectExtension).native());
    std::error_code ec;
    if (fs::is_regular_file(out.manifest, ec)) {
        return out; // already there: never touch user projects
    }
    if (!fs::is_directory(template_content, ec)) {
        return Error{ ErrorCode::NotFound, std::format("starter content not found: {}", utf8(template_content)) };
    }
    fs::create_directories(dir / "content", ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot create {}: {}", utf8(dir), ec.message()) };
    }
    // skip_existing: a half-created project (e.g. an interrupted first run) keeps what it has.
    fs::copy(template_content, dir / "content", fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot copy the starter content to {}: {}", utf8(dir), ec.message()) };
    }
    ProjectDesc p;
    p.name          = std::string(name);
    p.window.title  = p.name;
    p.startup_scene = startup_scene;
    p.content_root  = dir / "content";
    p.cooked_root   = dir / "assets";
    p.cooked_assets = false;
    {
        std::ofstream f(out.manifest, std::ios::binary | std::ios::trunc);
        f << project_to_json(p, dir);
        if (!f) {
            return Error{ ErrorCode::IoError, std::format("cannot write {}", utf8(out.manifest)) };
        }
    }
    out.created = true;
    AE_LOG_INFO("Workspace", "created project '{}' at {}", p.name, utf8(dir));
    return out;
}

Result<fs::path> default_user_project(const fs::path& engine_root) {
    const bool installed = is_installed_layout(engine_root);
    if (!installed && directory_writable(engine_root / "content")) {
        return fs::path{}; // development checkout: work in place
    }
    auto sp = ensure_starter_project(projects_dir(), engine_root / "content", kStarterProject, kStarterScene);
    if (!sp) {
        return sp.error();
    }
    return sp->manifest;
}

} // namespace aether::runtime
