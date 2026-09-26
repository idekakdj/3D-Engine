// asset_database.cpp — persistent asset DB (asset_db.json) + incremental import/cook.
#include "aether/assets/asset_database.h"

#include "aether/assets/format.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"

#include "file_util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace aether::assets {

namespace {

constexpr StringView kDbFormatName = "aether-asset-db";

String u64_to_hex(u64 v) { return std::format("{:016x}", v); }

std::optional<u64> u64_from_hex(StringView s) {
    u64 v = 0;
    if (s.empty() || s.size() > 16) return std::nullopt;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// Non-throwing JSON accessors.
const json* member(const json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
std::optional<String> get_string(const json& j, const char* key) {
    const json* m = member(j, key);
    if (!m || !m->is_string()) return std::nullopt;
    return m->get_ref<const String&>();
}
template <typename T>
std::optional<T> get_number(const json& j, const char* key) {
    const json* m = member(j, key);
    if (!m || !m->is_number_integer()) return std::nullopt;
    if constexpr (std::is_signed_v<T>) {
        return static_cast<T>(m->get<i64>());
    } else {
        if (m->is_number_unsigned()) return static_cast<T>(m->get<u64>());
        const i64 v = m->get<i64>();
        if (v < 0) return std::nullopt;
        return static_cast<T>(v);
    }
}

// "mesh:0" -> "mesh_0"; also neutralises drive colons of out-of-root absolute source paths.
String sanitize_path_component(StringView s) {
    String out(s);
    for (char& c : out) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\\') c = '_';
    }
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

String cooked_relative_path(StringView source_path, StringView sub_key) {
    return std::format("{}/{}{}", sanitize_path_component(source_path), sanitize_path_component(sub_key),
                       kCookedExtension);
}

} // namespace

std::optional<FileStamp> file_stamp(const fs::path& path) {
    std::error_code ec;
    const auto      time = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    return FileStamp{ static_cast<i64>(time.time_since_epoch().count()), static_cast<u64>(size) };
}

// ===========================================================================
// Impl
// ===========================================================================
struct AssetDatabase::Impl {
    fs::path       content_root;
    fs::path       cooked_root;
    ImportSettings settings;
    bool           is_open = false;

    mutable std::mutex                       mutex; // guards the three members below
    std::map<String, SourceRecord>           sources; // sorted by source path
    std::unordered_map<AssetId, AssetRecord> assets;
    mutable bool                             dirty = false;

    std::mutex                                              locks_mutex;
    std::unordered_map<String, std::shared_ptr<std::mutex>> source_locks;

    std::shared_ptr<std::mutex> lock_for(const String& source_path) {
        std::lock_guard lock(locks_mutex);
        auto& m = source_locks[source_path];
        if (!m) m = std::make_shared<std::mutex>();
        return m;
    }

    fs::path resolve(StringView path) const {
        fs::path p = detail::from_utf8(path);
        return p.is_absolute() ? p : content_root / p;
    }

    // Removes a source's records (caller holds `mutex`); returns cooked files to delete.
    std::vector<fs::path> erase_source_locked(const String& source_path) {
        std::vector<fs::path> files;
        auto it = sources.find(source_path);
        if (it == sources.end()) return files;
        for (const AssetId& id : it->second.assets) {
            auto a = assets.find(id);
            if (a == assets.end()) continue;
            files.push_back(cooked_root / detail::from_utf8(a->second.cooked_path));
            assets.erase(a);
        }
        sources.erase(it);
        dirty = true;
        return files;
    }

    Result<void> load_json(const fs::path& file);
    json         to_json() const;
};

Result<void> AssetDatabase::Impl::load_json(const fs::path& file) {
    const String text = [&] {
        auto bytes = detail::read_file_bytes(file);
        return bytes ? String(reinterpret_cast<const char*>(bytes->data()), bytes->size()) : String();
    }();
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        return Error{ ErrorCode::InvalidArgument, std::format("{}: not valid JSON", detail::to_utf8(file)) };
    }
    if (get_string(root, "format").value_or("") != kDbFormatName) {
        return Error{ ErrorCode::InvalidArgument, std::format("{}: not an asset database", detail::to_utf8(file)) };
    }
    if (get_number<u32>(root, "version").value_or(0) != kAssetDbVersion) {
        return Error{ ErrorCode::Unsupported, std::format("{}: unsupported database version", detail::to_utf8(file)) };
    }
    const json* list = member(root, "sources");
    if (!list || !list->is_array()) return {};

    usize skipped = 0;
    for (const json& s : *list) {
        SourceRecord sr;
        auto         path = get_string(s, "path");
        auto         hash = u64_from_hex(get_string(s, "hash").value_or(""));
        if (!path || !hash) {
            ++skipped;
            continue;
        }
        sr.source_path = *path;
        sr.source_hash = *hash;
        sr.importer_version = get_number<u32>(s, "importer_version").value_or(0);
        sr.stamp = { get_number<i64>(s, "mtime").value_or(0), get_number<u64>(s, "size").value_or(0) };
        sr.primary = asset_id_from_hex(get_string(s, "primary").value_or("")).value_or(AssetId{});
        if (const json* deps = member(s, "dependencies"); deps && deps->is_array()) {
            for (const json& d : *deps) {
                auto dp = get_string(d, "path");
                if (!dp) continue;
                sr.dependencies.push_back(*dp);
                sr.dependency_stamps.push_back({ get_number<i64>(d, "mtime").value_or(0), get_number<u64>(d, "size").value_or(0) });
            }
        }
        if (const json* as = member(s, "assets"); as && as->is_array()) {
            for (const json& a : *as) {
                AssetRecord rec;
                auto        id = asset_id_from_hex(get_string(a, "id").value_or(""));
                auto        type = parse_asset_type(get_string(a, "type").value_or(""));
                auto        key = get_string(a, "key");
                auto        cooked = get_string(a, "cooked");
                if (!id || !type || !key || !cooked) {
                    ++skipped;
                    continue;
                }
                rec.id = *id;
                rec.type = *type;
                rec.source_path = sr.source_path;
                rec.sub_key = *key;
                rec.name = get_string(a, "name").value_or(*key);
                rec.cooked_path = *cooked;
                rec.source_hash = sr.source_hash;
                rec.importer_version = sr.importer_version;
                sr.assets.push_back(rec.id);
                assets[rec.id] = std::move(rec);
            }
        }
        sources[sr.source_path] = std::move(sr);
    }
    if (skipped > 0) AE_LOG_WARN("Assets", "{}: skipped {} malformed record(s)", detail::to_utf8(file), skipped);
    return {};
}

json AssetDatabase::Impl::to_json() const {
    json root = json::object();
    root["format"] = kDbFormatName;
    root["version"] = kAssetDbVersion;
    json list = json::array();
    for (const auto& [path, sr] : sources) {
        json s = json::object();
        s["path"] = sr.source_path;
        s["hash"] = u64_to_hex(sr.source_hash);
        s["importer_version"] = sr.importer_version;
        s["mtime"] = sr.stamp.mtime;
        s["size"] = sr.stamp.size;
        s["primary"] = asset_id_to_hex(sr.primary);
        json deps = json::array();
        for (usize i = 0; i < sr.dependencies.size(); ++i) {
            const FileStamp st = i < sr.dependency_stamps.size() ? sr.dependency_stamps[i] : FileStamp{};
            deps.push_back(json{ { "path", sr.dependencies[i] }, { "mtime", st.mtime }, { "size", st.size } });
        }
        s["dependencies"] = std::move(deps);
        // Assets sorted by key for stable diffs.
        std::vector<const AssetRecord*> recs;
        for (const AssetId& id : sr.assets) {
            if (auto it = assets.find(id); it != assets.end()) recs.push_back(&it->second);
        }
        std::sort(recs.begin(), recs.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->sub_key < b->sub_key; });
        json as = json::array();
        for (const AssetRecord* r : recs) {
            as.push_back(json{ { "id", asset_id_to_hex(r->id) },
                               { "type", String(asset_type_name(r->type)) },
                               { "key", r->sub_key },
                               { "name", r->name },
                               { "cooked", r->cooked_path } });
        }
        s["assets"] = std::move(as);
        list.push_back(std::move(s));
    }
    root["sources"] = std::move(list);
    return root;
}

// ===========================================================================
// AssetDatabase
// ===========================================================================
AssetDatabase::AssetDatabase() : impl_(std::make_unique<Impl>()) {}
AssetDatabase::~AssetDatabase() = default;

Result<void> AssetDatabase::open(const fs::path& content_root, const fs::path& cooked_root,
                                 const ImportSettings& settings) {
    std::error_code ec;
    auto normalize = [&](const fs::path& p) {
        fs::path r = fs::weakly_canonical(fs::absolute(p, ec), ec);
        return (ec || r.empty()) ? fs::absolute(p, ec).lexically_normal() : r;
    };
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.mutex);
        d.content_root = normalize(content_root);
        d.cooked_root = normalize(cooked_root);
        d.settings = settings;
        d.settings.content_root = d.content_root;
        d.sources.clear();
        d.assets.clear();
        d.dirty = false;
    }
    fs::create_directories(d.cooked_root, ec);
    if (ec) {
        return Error{ ErrorCode::IoError, std::format("cannot create cooked directory {}: {}",
                                                      detail::to_utf8(d.cooked_root), ec.message()) };
    }
    d.is_open = true;
    const fs::path file = db_file();
    if (fs::exists(file, ec)) {
        std::lock_guard lock(d.mutex);
        if (auto r = d.load_json(file); !r) {
            AE_LOG_WARN("Assets", "{} - starting with an empty database (everything will re-import)", r.error().message);
            d.sources.clear();
            d.assets.clear();
            d.dirty = true;
        }
    }
    return {};
}

bool AssetDatabase::is_open() const noexcept { return impl_->is_open; }

Result<void> AssetDatabase::save() const {
    String text;
    {
        std::lock_guard lock(impl_->mutex);
        text = impl_->to_json().dump(2, ' ', false, json::error_handler_t::replace);
        impl_->dirty = false;
    }
    text.push_back('\n');
    auto r = detail::write_file_atomic(db_file(), detail::bytes_of(text));
    if (!r) {
        std::lock_guard lock(impl_->mutex);
        impl_->dirty = true;
    }
    return r;
}

Result<void> AssetDatabase::save_if_dirty() const {
    return dirty() ? save() : Result<void>{};
}

bool AssetDatabase::dirty() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->dirty;
}

Result<u64> AssetDatabase::compute_source_hash(StringView source_path, const std::vector<String>& dependencies) const {
    const u64 fp = impl_->settings.fingerprint();
    u8        fp_bytes[8];
    for (int i = 0; i < 8; ++i) fp_bytes[i] = static_cast<u8>(fp >> (8 * i));
    u64 h = fnv1a64(ByteSpan(reinterpret_cast<const byte*>(fp_bytes), 8));

    auto bytes = detail::read_file_bytes(impl_->resolve(source_path));
    if (!bytes) return bytes.error();
    h = fnv1a64(detail::bytes_of(*bytes), h);
    for (const String& dep : dependencies) {
        h = fnv1a64(dep, h);
        auto dep_bytes = detail::read_file_bytes(impl_->resolve(dep));
        h = dep_bytes ? fnv1a64(detail::bytes_of(*dep_bytes), h) : fnv1a64(StringView("\0<missing>", 10), h);
    }
    return h;
}

ImportOutcome AssetDatabase::import_source(const fs::path& source, bool force, ImportResult* out_result) {
    Impl&          d = *impl_;
    const fs::path abs = source.is_absolute() ? source : d.content_root / source;
    ImportOutcome  oc;
    oc.source_path = to_source_path(abs);
    const String& rel = oc.source_path;

    const auto      source_lock = d.lock_for(rel);
    std::lock_guard serialize(*source_lock);

    const auto stamp = file_stamp(abs);
    if (!stamp) {
        oc.error = Error{ ErrorCode::NotFound, std::format("source not found: {}", detail::to_utf8(abs)) };
        return oc;
    }
    auto dep_stamps_of = [&](const std::vector<String>& deps) {
        std::vector<FileStamp> stamps;
        for (const String& dep : deps) stamps.push_back(file_stamp(d.resolve(dep)).value_or(FileStamp{}));
        return stamps;
    };

    // ---- incremental check ----
    if (!force) {
        if (auto prev = find_source(rel); prev && prev->importer_version == kImporterVersion) {
            auto hash = compute_source_hash(rel, prev->dependencies);
            bool fresh = hash && *hash == prev->source_hash;
            for (const AssetRecord& rec : assets_of_source(rel)) {
                std::error_code ec;
                if (!fresh) break;
                fresh = fs::exists(cooked_file(rec), ec);
            }
            if (fresh) {
                const std::vector<FileStamp> dep_stamps = dep_stamps_of(prev->dependencies);
                std::lock_guard              lock(d.mutex);
                if (auto it = d.sources.find(rel); it != d.sources.end() &&
                                                   (it->second.stamp != *stamp || it->second.dependency_stamps != dep_stamps)) {
                    it->second.stamp = *stamp; // touched but unchanged: refresh stamps only
                    it->second.dependency_stamps = dep_stamps;
                    d.dirty = true;
                }
                oc.status = ImportStatus::UpToDate;
                return oc;
            }
        }
    }

    // ---- import ----
    auto imported = import_file(abs, d.settings);
    if (!imported) {
        oc.error = imported.error();
        return oc;
    }
    ImportResult& res = *imported;
    std::vector<String> deps;
    for (const fs::path& p : res.dependencies) deps.push_back(to_source_path(p));
    auto hash = compute_source_hash(rel, deps);
    if (!hash) {
        oc.error = hash.error();
        return oc;
    }

    // ---- cook ----
    std::vector<AssetRecord> records;
    Error                    failure;
    bool                     failed = false;
    res.for_each_asset([&](const auto& a) {
        if (failed) return;
        using T = std::remove_cvref_t<decltype(a.data)>;
        AssetRecord rec;
        rec.id = a.id;
        rec.type = asset_type_of_v<T>;
        rec.source_path = rel;
        rec.sub_key = a.key;
        rec.name = a.name;
        rec.cooked_path = cooked_relative_path(rel, a.key);
        rec.source_hash = *hash;
        rec.importer_version = kImporterVersion;
        {
            std::lock_guard lock(d.mutex);
            if (auto it = d.assets.find(a.id);
                it != d.assets.end() && (it->second.source_path != rel || it->second.sub_key != a.key)) {
                failure = Error{ ErrorCode::AlreadyExists,
                                 std::format("AssetId collision: {}#{} vs {}#{}", rel, a.key, it->second.source_path,
                                             it->second.sub_key) };
                failed = true;
                return;
            }
        }
        if (auto w = write_asset<T>(cooked_file(rec), CookedMeta{ a.id, *hash, kImporterVersion }, a.data); !w) {
            failure = w.error();
            failed = true;
            return;
        }
        records.push_back(std::move(rec));
    });
    if (failed) {
        oc.error = failure;
        return oc;
    }

    // ---- commit ----
    std::vector<fs::path> stale_files;
    {
        std::unordered_set<AssetId> fresh_ids;
        for (const AssetRecord& r : records) fresh_ids.insert(r.id);
        SourceRecord sr;
        sr.source_path = rel;
        sr.source_hash = *hash;
        sr.importer_version = kImporterVersion;
        sr.stamp = *stamp;
        sr.dependency_stamps = dep_stamps_of(deps);
        sr.dependencies = std::move(deps);
        sr.primary = res.primary;
        for (const AssetRecord& r : records) sr.assets.push_back(r.id);

        std::lock_guard lock(d.mutex);
        if (auto it = d.sources.find(rel); it != d.sources.end()) {
            for (const AssetId& id : it->second.assets) {
                if (fresh_ids.contains(id)) continue;
                if (auto a = d.assets.find(id); a != d.assets.end()) {
                    stale_files.push_back(d.cooked_root / detail::from_utf8(a->second.cooked_path));
                    d.assets.erase(a);
                }
            }
        }
        for (AssetRecord& r : records) d.assets[r.id] = r;
        d.sources[rel] = std::move(sr);
        d.dirty = true;
    }
    for (const fs::path& f : stale_files) {
        std::error_code ec;
        fs::remove(f, ec);
    }

    oc.status = ImportStatus::Imported;
    oc.assets_written = static_cast<u32>(records.size());
    oc.warnings = res.warnings;
    if (out_result) *out_result = std::move(res);
    return oc;
}

ScanReport AssetDatabase::scan(const ScanOptions& options) {
    Impl&      d = *impl_;
    ScanReport report;

    std::vector<fs::path> files;
    std::error_code       ec;
    if (fs::is_directory(d.content_root, ec)) {
        const auto opts = fs::directory_options::skip_permission_denied;
        for (auto it = fs::recursive_directory_iterator(d.content_root, opts, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            std::error_code type_ec;
            if (it->is_directory(type_ec)) {
                const String name = detail::to_utf8(p.filename());
                if ((!name.empty() && name[0] == '.') || p == d.cooked_root) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(type_ec) && is_supported_source(p)) files.push_back(p);
        }
    }
    std::vector<std::pair<String, fs::path>> sorted;
    sorted.reserve(files.size());
    for (const fs::path& f : files) sorted.emplace_back(to_source_path(f), f);
    std::sort(sorted.begin(), sorted.end());

    report.sources_found = static_cast<u32>(sorted.size());
    report.outcomes.resize(sorted.size());
    auto work = [&](u32 i) { report.outcomes[i] = import_source(sorted[i].second, options.force); };
    if (options.parallel && sorted.size() > 1) {
        JobCounter counter;
        JobSystem::parallel_for(static_cast<u32>(sorted.size()), 1, work, &counter);
        JobSystem::wait(counter);
    } else {
        for (u32 i = 0; i < sorted.size(); ++i) work(i);
    }

    if (options.prune_missing) {
        std::unordered_set<String> found;
        for (const auto& [rel, path] : sorted) found.insert(rel);
        for (const String& src : all_sources()) {
            if (found.contains(src)) continue;
            std::error_code exists_ec;
            if (!fs::exists(source_file(src), exists_ec) && remove_source(src)) ++report.removed;
        }
    }

    for (const ImportOutcome& oc : report.outcomes) {
        switch (oc.status) {
        case ImportStatus::Imported:
            ++report.imported;
            report.assets_written += oc.assets_written;
            break;
        case ImportStatus::UpToDate: ++report.up_to_date; break;
        case ImportStatus::Failed: ++report.failed; break;
        }
    }
    return report;
}

bool AssetDatabase::remove_source(StringView source_path) {
    std::vector<fs::path> files;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->sources.contains(String(source_path))) return false;
        files = impl_->erase_source_locked(String(source_path));
    }
    for (const fs::path& f : files) {
        std::error_code ec;
        fs::remove(f, ec);
    }
    return true;
}

bool AssetDatabase::stamps_match(StringView source_path) const {
    const auto sr = find_source(source_path);
    if (!sr) return false;
    if (file_stamp(impl_->resolve(source_path)).value_or(FileStamp{ -1, 0 }) != sr->stamp) return false;
    for (usize i = 0; i < sr->dependencies.size(); ++i) {
        const FileStamp expected = i < sr->dependency_stamps.size() ? sr->dependency_stamps[i] : FileStamp{};
        if (file_stamp(impl_->resolve(sr->dependencies[i])).value_or(FileStamp{}) != expected) return false;
    }
    return true;
}

std::optional<AssetRecord> AssetDatabase::find(AssetId id) const {
    std::lock_guard lock(impl_->mutex);
    auto            it = impl_->assets.find(id);
    if (it == impl_->assets.end()) return std::nullopt;
    return it->second;
}

std::optional<AssetRecord> AssetDatabase::find(StringView source_path, StringView sub_key) const {
    std::lock_guard lock(impl_->mutex);
    auto            s = impl_->sources.find(String(source_path));
    if (s == impl_->sources.end()) return std::nullopt;
    for (const AssetId& id : s->second.assets) {
        auto a = impl_->assets.find(id);
        if (a != impl_->assets.end() && a->second.sub_key == sub_key) return a->second;
    }
    return std::nullopt;
}

std::optional<SourceRecord> AssetDatabase::find_source(StringView source_path) const {
    std::lock_guard lock(impl_->mutex);
    auto            s = impl_->sources.find(String(source_path));
    if (s == impl_->sources.end()) return std::nullopt;
    return s->second;
}

std::vector<AssetRecord> AssetDatabase::assets_of_source(StringView source_path) const {
    std::vector<AssetRecord> out;
    std::lock_guard          lock(impl_->mutex);
    auto                     s = impl_->sources.find(String(source_path));
    if (s == impl_->sources.end()) return out;
    for (const AssetId& id : s->second.assets) {
        if (auto a = impl_->assets.find(id); a != impl_->assets.end()) out.push_back(a->second);
    }
    return out;
}

AssetId AssetDatabase::primary_asset(StringView source_path) const {
    std::lock_guard lock(impl_->mutex);
    auto            s = impl_->sources.find(String(source_path));
    return s == impl_->sources.end() ? AssetId{} : s->second.primary;
}

std::vector<AssetRecord> AssetDatabase::all_assets() const {
    std::vector<AssetRecord> out;
    {
        std::lock_guard lock(impl_->mutex);
        out.reserve(impl_->assets.size());
        for (const auto& [id, rec] : impl_->assets) out.push_back(rec);
    }
    std::sort(out.begin(), out.end(), [](const AssetRecord& a, const AssetRecord& b) { return a.id < b.id; });
    return out;
}

std::vector<String> AssetDatabase::all_sources() const {
    std::vector<String> out;
    std::lock_guard     lock(impl_->mutex);
    for (const auto& [path, sr] : impl_->sources) out.push_back(path);
    return out;
}

usize AssetDatabase::asset_count() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->assets.size();
}

const fs::path& AssetDatabase::content_root() const noexcept { return impl_->content_root; }
const fs::path& AssetDatabase::cooked_root() const noexcept { return impl_->cooked_root; }
fs::path        AssetDatabase::db_file() const { return impl_->cooked_root / kAssetDbFileName; }
fs::path        AssetDatabase::cooked_file(const AssetRecord& r) const {
    return impl_->cooked_root / detail::from_utf8(r.cooked_path);
}
fs::path AssetDatabase::source_file(StringView source_path) const { return impl_->resolve(source_path); }
String   AssetDatabase::to_source_path(const fs::path& file) const {
    return canonical_source_path(file, impl_->content_root);
}
const ImportSettings& AssetDatabase::import_settings() const noexcept { return impl_->settings; }

} // namespace aether::assets
