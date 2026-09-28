// project.cpp — .aeproject manifests and packaging (see aether/runtime/project.h).
#include "aether/runtime/project.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/importers.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/gameplay/application.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace aether::runtime {
namespace {

constexpr const char* kFormat  = "aether.project";
constexpr int         kVersion = 1;

std::string to_utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Reads `key` from `obj` into `out` when present and of the right type; records a type error.
template <class T>
void read(const json& obj, const char* key, T& out, std::string& error) {
    auto it = obj.find(key);
    if (it == obj.end() || !error.empty()) {
        return;
    }
    try {
        out = it->template get<T>();
    } catch (const json::exception&) {
        error = std::format("project key '{}' has the wrong type", key);
    }
}

void read_path(const json& obj, const char* key, fs::path& out, std::string& error) {
    std::string s;
    read(obj, key, s, error);
    if (!s.empty()) {
        out = from_utf8(s);
    }
}

void warn_unknown(const json& obj, std::initializer_list<const char*> known, const char* where) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (std::none_of(known.begin(), known.end(), [&](const char* k) { return it.key() == k; })) {
            AE_LOG_WARN("Project", "unknown key '{}' in {} (ignored)", it.key(), where);
        }
    }
}

fs::path resolve(const fs::path& p, const fs::path& base) {
    if (p.empty()) {
        return {};
    }
    return (p.is_absolute() ? p : base / p).lexically_normal();
}

// `p` relative to `base` when it lies below it, else `p` itself.
fs::path relative_if_below(const fs::path& p, const fs::path& base) {
    if (p.empty() || base.empty()) {
        return p;
    }
    const fs::path rel = p.lexically_normal().lexically_relative(base.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") {
        return p;
    }
    return rel;
}

Result<void> copy_one(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot create {}: {}", to_utf8(to.parent_path()), ec.message()) };
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot copy {} -> {}: {}", to_utf8(from), to_utf8(to), ec.message()) };
    }
    return {};
}

// Copies every regular file under `from` accepted by `filter` to the same relative path under
// `to`. Skips `exclude` (e.g. the output directory when it is nested in the source tree).
Result<u32> copy_tree(const fs::path& from, const fs::path& to, const fs::path& exclude,
                      bool (*filter)(const fs::path&)) {
    std::error_code ec;
    u32             copied = 0;
    const fs::path  skip   = exclude.empty() ? fs::path{} : fs::weakly_canonical(exclude, ec);
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            const std::string name = to_utf8(p.filename());
            if ((!name.empty() && name[0] == '.') || (!skip.empty() && fs::weakly_canonical(p, ec) == skip)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec) || (filter != nullptr && !filter(p))) {
            continue;
        }
        if (auto r = copy_one(p, to / p.lexically_relative(from)); !r) {
            return r.error();
        }
        ++copied;
    }
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot walk {}: {}", to_utf8(from), ec.message()) };
    }
    return copied;
}

} // namespace

// =================================================================================================
// manifest
// =================================================================================================
Result<ProjectDesc> parse_project(StringView text, const fs::path& base_dir) {
    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (j.is_discarded() || !j.is_object()) {
        return Error{ ErrorCode::InvalidArgument, "project manifest is not a JSON object" };
    }
    if (j.value("format", std::string{}) != kFormat) {
        return Error{ ErrorCode::InvalidArgument, std::format("project manifest: \"format\" must be \"{}\"", kFormat) };
    }
    if (const int version = j.value("version", kVersion); version > kVersion) {
        return Error{ ErrorCode::Unsupported,
                      std::format("project manifest version {} is newer than supported ({})", version, kVersion) };
    }
    warn_unknown(j,
                 { "format", "version", "name", "startup_scene", "content_root", "cooked_root", "cooked_assets",
                   "input_map", "window", "simulation", "rendering", "camera_controller" },
                 "project");

    ProjectDesc p;
    std::string error;
    read(j, "name", p.name, error);
    read_path(j, "startup_scene", p.startup_scene, error);
    read_path(j, "content_root", p.content_root, error);
    read_path(j, "cooked_root", p.cooked_root, error);
    read(j, "cooked_assets", p.cooked_assets, error);
    read_path(j, "input_map", p.input_map, error);
    read(j, "camera_controller", p.camera_controller, error);
    p.window.title = p.name;
    if (auto it = j.find("window"); it != j.end() && it->is_object()) {
        warn_unknown(*it, { "title", "width", "height", "resizable", "vsync", "fullscreen" }, "window");
        read(*it, "title", p.window.title, error);
        read(*it, "width", p.window.width, error);
        read(*it, "height", p.window.height, error);
        read(*it, "resizable", p.window.resizable, error);
        read(*it, "vsync", p.window.vsync, error);
        read(*it, "fullscreen", p.fullscreen, error);
    }
    if (auto it = j.find("simulation"); it != j.end() && it->is_object()) {
        warn_unknown(*it, { "fixed_delta", "max_fixed_steps" }, "simulation");
        read(*it, "fixed_delta", p.fixed_delta, error);
        read(*it, "max_fixed_steps", p.max_fixed_steps, error);
    }
    if (auto it = j.find("rendering"); it != j.end() && it->is_object()) {
        warn_unknown(*it, { "exposure", "sky" }, "rendering");
        read(*it, "exposure", p.exposure, error);
        read(*it, "sky", p.sky, error);
    }
    if (!error.empty()) {
        return Error{ ErrorCode::InvalidArgument, error };
    }
    if (p.window.width == 0 || p.window.height == 0 || p.window.width > 16384 || p.window.height > 16384) {
        return Error{ ErrorCode::InvalidArgument, "project window size out of range" };
    }
    if (!(p.fixed_delta > 0.0f && p.fixed_delta <= 1.0f) || p.max_fixed_steps == 0) {
        return Error{ ErrorCode::InvalidArgument, "project simulation settings out of range" };
    }
    if (!(p.exposure > 0.0f)) {
        return Error{ ErrorCode::InvalidArgument, "project exposure must be positive" };
    }
    p.content_root = resolve(p.content_root, base_dir);
    p.cooked_root  = resolve(p.cooked_root, base_dir);
    return p;
}

Result<ProjectDesc> load_project(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Error{ ErrorCode::NotFound, std::format("cannot open project {}", to_utf8(file)) };
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::error_code   ec;
    const fs::path    dir = fs::absolute(file, ec).parent_path();
    auto              r   = parse_project(text, dir);
    if (!r) {
        return Error{ r.error().code, std::format("{}: {}", to_utf8(file), r.error().message) };
    }
    return r;
}

std::string project_to_json(const ProjectDesc& p, const fs::path& base_dir) {
    json j;
    j["format"]  = kFormat;
    j["version"] = kVersion;
    j["name"]    = p.name;
    if (!p.startup_scene.empty()) j["startup_scene"] = to_utf8(p.startup_scene);
    if (!p.content_root.empty()) j["content_root"] = to_utf8(relative_if_below(p.content_root, base_dir));
    if (!p.cooked_root.empty()) j["cooked_root"] = to_utf8(relative_if_below(p.cooked_root, base_dir));
    j["cooked_assets"] = p.cooked_assets;
    if (!p.input_map.empty()) j["input_map"] = to_utf8(p.input_map);
    j["window"] = { { "title", p.window.title },         { "width", p.window.width },
                    { "height", p.window.height },       { "resizable", p.window.resizable },
                    { "vsync", p.window.vsync },         { "fullscreen", p.fullscreen } };
    j["simulation"]        = { { "fixed_delta", p.fixed_delta }, { "max_fixed_steps", p.max_fixed_steps } };
    j["rendering"]         = { { "exposure", p.exposure }, { "sky", p.sky } };
    j["camera_controller"] = p.camera_controller;
    return j.dump(2) + "\n";
}

fs::path find_default_project(const fs::path& executable_dir, const fs::path& engine_root) {
    for (const fs::path& dir : { executable_dir, engine_root }) {
        std::error_code       ec;
        std::vector<fs::path> found;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && lower(to_utf8(it->path().extension())) == kProjectExtension) {
                found.push_back(it->path());
            }
        }
        if (found.empty()) {
            continue;
        }
        std::sort(found.begin(), found.end());
        for (const fs::path& f : found) {
            if (f.filename() == "game.aeproject") {
                return f;
            }
        }
        if (found.size() > 1) {
            AE_LOG_WARN("Project", "{} projects in {}; using {}", found.size(), to_utf8(dir), to_utf8(found.front()));
        }
        return found.front();
    }
    return {};
}

void apply_project(const ProjectDesc& p, gameplay::AppDesc& desc) {
    desc.window             = p.window;
    desc.vsync              = p.window.vsync;
    desc.fixed_delta        = p.fixed_delta;
    desc.max_fixed_steps    = p.max_fixed_steps;
    desc.content_root       = p.content_root;
    desc.cooked_root        = p.cooked_root;
    desc.cooked_assets_only = p.cooked_assets;
    desc.startup_scene      = p.startup_scene;
}

// =================================================================================================
// packaging
// =================================================================================================
bool is_runtime_content_file(const fs::path& file) {
    const std::string ext = lower(to_utf8(file.extension()));
    return ext == ".aescene" || ext == ".aeprefab" || ext == ".lua" || ext == ".aegraph" || ext == ".json"; // .aegraph: ADR-0018
}

Result<PackageReport> package_project(const PackageOptions& o) {
    auto loaded = load_project(o.project_file);
    if (!loaded) {
        return loaded.error();
    }
    ProjectDesc     project = std::move(*loaded);
    std::error_code ec;
    const fs::path  out = fs::absolute(o.output_dir, ec).lexically_normal();
    fs::create_directories(out, ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot create {}: {}", to_utf8(out), ec.message()) };
    }
    const fs::path content = project.content_root.empty() ? paths::content_dir() : project.content_root;
    if (!fs::is_directory(content, ec)) {
        return Error{ ErrorCode::NotFound, std::format("content directory not found: {}", to_utf8(content)) };
    }
    if (!project.startup_scene.empty() && !project.startup_scene.is_absolute() &&
        !fs::is_regular_file(content / project.startup_scene, ec)) {
        return Error{ ErrorCode::NotFound,
                      std::format("startup scene not found: {}", to_utf8(content / project.startup_scene)) };
    }

    PackageReport report;

    // 1. Cook every importable source into <out>/assets.
    const fs::path      cooked = out / "assets";
    assets::AssetDatabase db;
    if (auto r = db.open(content, cooked); !r) {
        return r.error();
    }
    assets::ScanOptions scan;
    scan.force    = o.force_cook;
    scan.parallel = JobSystem::worker_count() > 0;
    const assets::ScanReport sr = db.scan(scan);
    report.sources_cooked = sr.imported + sr.up_to_date;
    report.sources_failed = sr.failed;
    for (const assets::ImportOutcome& oc : sr.outcomes) {
        if (oc.status == assets::ImportStatus::Failed) {
            report.errors.push_back(std::format("{}: {}", oc.source_path, oc.error.message));
        }
    }
    if (auto r = db.save(); !r) {
        return r.error();
    }
    report.assets = db.asset_count();

    // 2. Runtime-loaded content (scenes, scripts, data).
    auto copied = copy_tree(content, out / "content", out, &is_runtime_content_file);
    if (!copied) {
        return copied.error();
    }
    report.files_copied += *copied;

    // 3. Shaders (compiled at startup) and the player itself.
    if (!o.shader_dir.empty()) {
        auto s = copy_tree(o.shader_dir, out / "shaders", out, nullptr);
        if (!s) {
            return s.error();
        }
        report.files_copied += *s;
    }
    if (!o.executable.empty()) {
        const fs::path dst = out / o.executable.filename();
        if (fs::weakly_canonical(o.executable, ec) != fs::weakly_canonical(dst, ec)) {
            if (auto r = copy_one(o.executable, dst); !r) {
                return r.error();
            }
            fs::permissions(dst, fs::status(o.executable, ec).permissions(), ec);
            ++report.files_copied;
        }
    }

    // 4. The shipping manifest: roots next to the player, cooked data only.
    project.content_root  = out / "content";
    project.cooked_root   = cooked;
    project.cooked_assets = true;
    report.manifest       = out / "game.aeproject";
    {
        std::ofstream f(report.manifest, std::ios::binary | std::ios::trunc);
        f << project_to_json(project, out);
        if (!f) {
            return Error{ ErrorCode::IoError, std::format("cannot write {}", to_utf8(report.manifest)) };
        }
    }
    return report;
}

} // namespace aether::runtime
