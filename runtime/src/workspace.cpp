// workspace.cpp — see aether/runtime/workspace.h.
#include "aether/runtime/workspace.h"

#include "aether/core/log.h"
#include "aether/core/paths_ext.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/component_codecs.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/runtime/project.h"
#include "aether/scene/components.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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
#else
#    include <spawn.h>
#    include <unistd.h>
extern char** environ;
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

// =================================================================================================
// project management (ADR-0014)
// =================================================================================================
bool valid_project_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ' || name.back() == '.' ||
        name.front() == '.') {
        return false;
    }
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) {
            return false;
        }
    }
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static constexpr std::array<std::string_view, 22> kReserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    return std::find(kReserved.begin(), kReserved.end(), upper) == kReserved.end();
}

namespace {
// A minimal scene that shows something: camera, sun, floor.
Result<void> write_empty_scene(const fs::path& file) {
    World w;
    gameplay::register_default_codecs(w);
    const Entity cam = w.create("Camera");
    w.add<CameraComponent>(cam, CameraComponent{ 60.0f, 0.1f, 500.0f, true });
    scene::set_local_position(w, cam, Vec3(0.0f, 3.0f, 8.0f));
    scene::set_local_rotation(w, cam, gameplay::rotation_from_yaw_pitch(0.0f, -18.0f));
    const Entity sun = w.create("Sun");
    LightComponent l;
    l.kind         = LightKind::Directional;
    l.intensity    = 3.0f;
    l.cast_shadows = true;
    w.add<LightComponent>(sun, l);
    scene::set_local_rotation(w, sun, gameplay::rotation_from_yaw_pitch(35.0f, -50.0f));
    const Entity floor = w.create("Floor");
    MeshRendererComponent mr;
    mr.mesh     = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Plane);
    mr.material = gameplay::builtin_material_id(gameplay::BuiltinMaterial::Default);
    w.add<MeshRendererComponent>(floor, mr);
    w.update_transforms();
    return scene::save_scene(w, file);
}
} // namespace

Result<fs::path> create_project(const fs::path& parent, std::string_view name, ProjectTemplate tmpl,
                                const fs::path& starter_content) {
    if (!valid_project_name(name)) {
        return Error{ ErrorCode::InvalidArgument, std::format("'{}' is not a valid project name", name) };
    }
    const fs::path  dir = parent / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    std::error_code ec;
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) {
        return Error{ ErrorCode::AlreadyExists, std::format("{} already exists and is not empty", utf8(dir)) };
    }
    if (tmpl == ProjectTemplate::Starter) {
        auto sp = ensure_starter_project(parent, starter_content, name, kStarterScene);
        if (!sp) {
            return sp.error();
        }
        return sp->manifest;
    }
    fs::create_directories(dir / "content" / "scenes", ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot create {}: {}", utf8(dir), ec.message()) };
    }
    fs::create_directories(dir / "content" / "scripts", ec);
    if (auto r = write_empty_scene(dir / "content" / "scenes" / "main.aescene"); !r) {
        return r.error();
    }
    ProjectDesc p;
    p.name          = std::string(name);
    p.window.title  = p.name;
    p.startup_scene = "scenes/main.aescene";
    p.content_root  = dir / "content";
    p.cooked_root   = dir / "assets";
    const fs::path manifest = dir / (dir.filename().native() + fs::path(kProjectExtension).native());
    std::ofstream  f(manifest, std::ios::binary | std::ios::trunc);
    f << project_to_json(p, dir);
    if (!f) {
        return Error{ ErrorCode::IoError, std::format("cannot write {}", utf8(manifest)) };
    }
    AE_LOG_INFO("Workspace", "created empty project '{}' at {}", p.name, utf8(dir));
    return manifest;
}

std::vector<ProjectEntry> find_projects(const fs::path& dir) {
    std::vector<ProjectEntry> out;
    std::error_code           ec;
    auto consider = [&](const fs::path& f) {
        if (f.extension() == kProjectExtension && fs::is_regular_file(f, ec)) {
            auto p = load_project(f);
            out.push_back(ProjectEntry{ p ? p->name : utf8(f.stem()), f });
        }
    };
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            std::error_code ec2;
            for (auto sub = fs::directory_iterator(it->path(), ec2); !ec2 && sub != fs::directory_iterator(); sub.increment(ec2)) {
                consider(sub->path());
            }
        } else {
            consider(it->path());
        }
    }
    std::sort(out.begin(), out.end(), [](const ProjectEntry& a, const ProjectEntry& b) { return a.name < b.name; });
    return out;
}

RecentProjects::RecentProjects(fs::path file) : file_(std::move(file)) {}

fs::path RecentProjects::default_file() { return paths::cache_dir() / "recent_projects.json"; }

void RecentProjects::load() {
    entries_.clear();
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("projects") || !j["projects"].is_array()) {
        return;
    }
    std::error_code ec;
    for (const auto& e : j["projects"]) {
        if (e.is_string()) {
            const std::string s = e.get<std::string>();
            const fs::path    p(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
            if (fs::is_regular_file(p, ec) && entries_.size() < kMaxRecent &&
                std::find(entries_.begin(), entries_.end(), p) == entries_.end()) {
                entries_.push_back(p);
            }
        }
    }
}

Result<void> RecentProjects::save() const {
    nlohmann::json j;
    j["projects"] = nlohmann::json::array();
    for (const fs::path& p : entries_) {
        j["projects"].push_back(utf8(p));
    }
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    std::ofstream f(file_, std::ios::binary | std::ios::trunc);
    f << j.dump(2) << "\n";
    if (!f) {
        return Error{ ErrorCode::IoError, std::format("cannot write {}", utf8(file_)) };
    }
    return {};
}

void RecentProjects::add(const fs::path& manifest) {
    std::error_code ec;
    fs::path        p = fs::weakly_canonical(fs::absolute(manifest, ec), ec);
    if (p.empty()) {
        p = manifest;
    }
    remove(p);
    entries_.insert(entries_.begin(), p);
    if (entries_.size() > kMaxRecent) {
        entries_.resize(kMaxRecent);
    }
}

void RecentProjects::remove(const fs::path& manifest) {
    std::erase_if(entries_, [&](const fs::path& e) { return e == manifest; });
}

fs::path executable_path() {
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

Result<void> launch_detached(const fs::path& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    // Quote every argument (CommandLineToArgvW rules: backslashes before a quote are doubled).
    auto quote = [](const std::wstring& a) {
        std::wstring q = L"\"";
        std::size_t  slashes = 0;
        for (const wchar_t c : a) {
            if (c == L'\\') {
                ++slashes;
            } else if (c == L'"') {
                q.append(slashes * 2 + 1, L'\\');
                slashes = 0;
            } else {
                q.append(slashes, L'\\');
                slashes = 0;
            }
            if (c != L'\\') {
                q.push_back(c);
            }
        }
        q.append(slashes * 2, L'\\');
        q.push_back(L'"');
        return q;
    };
    std::wstring cmd = quote(exe.native());
    for (const std::string& a : args) {
        const fs::path w(std::u8string(reinterpret_cast<const char8_t*>(a.data()), a.size()));
        cmd += L" " + quote(w.native());
    }
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi)) {
        return Error{ ErrorCode::IoError, std::format("cannot start {} (error {})", utf8(exe), GetLastError()) };
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
#else
    const std::string        exe_s = exe.string();
    std::vector<std::string> owned{ exe_s };
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : owned) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (const int rc = posix_spawn(&pid, exe_s.c_str(), nullptr, nullptr, argv.data(), environ); rc != 0) {
        return Error{ ErrorCode::IoError, std::format("cannot start {} (error {})", exe_s, rc) };
    }
    return {};
#endif
}

} // namespace aether::runtime
