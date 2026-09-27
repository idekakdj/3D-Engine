// aether/assets/asset_database.h — the persistent asset database + incremental cooker.
//
// Maps AssetId <-> {type, content-relative source path, sub-asset key, cooked path,
// source hash, importer version} and persists it as `<cooked_root>/asset_db.json`
// (sorted, diff-friendly JSON). It owns the import -> cook step:
//
//   import_source(file)   fast path: if kImporterVersion, the import-settings fingerprint and
//                         the size + mtime of the source and every recorded dependency match
//                         the record and every cooked file exists -> UpToDate WITHOUT hashing.
//                         Otherwise hash source (+ dependencies + settings): an equal hash is
//                         still UpToDate (stamps are refreshed; the hash is authoritative
//                         whenever stamps differ); else import, write one .aeasset per
//                         sub-asset under <cooked_root>/<source_path>/<key>.aeasset, and
//                         replace the source's records (stale sub-assets are removed).
//                         A (re)imported source's ImportResult::referenced_sources (shared
//                         standalone images) are imported too.
//                         Trade-off: an edit that keeps both size and mtime is not detected
//                         (use force / --force); every other change is.
//   scan()                import every supported source under content_root (in parallel
//                         through aether::JobSystem when requested) and prune records of
//                         deleted sources.
//
// Thread-safety: after open(), every member function is thread-safe. Imports of the same
// source are serialised; imports of different sources run concurrently. Query functions
// return copies (never references into the database) for that reason.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/assets/importers.h"
#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aether::assets {

inline constexpr const char* kAssetDbFileName = "asset_db.json";
inline constexpr u32         kAssetDbVersion = 1;

// Last-write time + size of a file, used for cheap change detection (hot reload).
struct FileStamp {
    i64 mtime = 0; // std::filesystem::file_time_type ticks
    u64 size = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

struct AssetRecord {
    AssetId   id;
    AssetType type = AssetType::Unknown;
    String    source_path;  // content-relative, '/'-separated, UTF-8
    String    sub_key;      // e.g. "mesh:0"
    String    name;         // display name
    String    cooked_path;  // cooked-root-relative, '/'-separated
    u64       source_hash = 0;
    u32       importer_version = 0;
};

struct SourceRecord {
    String              source_path;
    u64                 source_hash = 0;
    u32                 importer_version = 0;
    FileStamp           stamp;               // of the source file at import time
    u64                 settings_fingerprint = 0; // ImportSettings::fingerprint() at import time
    std::vector<String> dependencies;        // content-relative (or absolute) paths
    std::vector<FileStamp> dependency_stamps; // parallel to `dependencies`
    AssetId             primary;
    std::vector<AssetId> assets;
};

enum class ImportStatus : u8 { Imported = 0, UpToDate, Failed };

struct ImportOutcome {
    String              source_path;
    ImportStatus        status = ImportStatus::Failed;
    u32                 assets_written = 0;
    Error               error;    // meaningful when status == Failed
    std::vector<String> warnings; // importer warnings (Imported only)
    bool                hashed = false; // the incremental check had to hash (stamps differed)
};

struct ScanOptions {
    bool force = false;         // re-import even when up to date
    bool parallel = true;       // use aether::JobSystem (must be initialised)
    bool prune_missing = true;  // drop records + cooked files of deleted sources
};

struct ScanReport {
    u32 sources_found = 0;
    u32 imported = 0;
    u32 up_to_date = 0;
    u32 failed = 0;
    u32 removed = 0;        // pruned sources
    u32 assets_written = 0;
    u32 hashed = 0;         // sources whose up-to-date check needed a content hash
    std::vector<ImportOutcome> outcomes; // one per source found, sorted by source path
};

class AssetDatabase {
public:
    AssetDatabase();
    ~AssetDatabase();
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    // Binds the database to a content root (sources) and cooked root (outputs) and loads
    // `<cooked_root>/asset_db.json` if present (a missing file = empty database). The
    // cooked root is created on demand. Not thread-safe: call before sharing the object.
    [[nodiscard]] Result<void> open(const std::filesystem::path& content_root,
                                    const std::filesystem::path& cooked_root,
                                    const ImportSettings&        settings = {});
    [[nodiscard]] bool is_open() const noexcept;

    // Writes asset_db.json atomically. Thread-safe.
    [[nodiscard]] Result<void> save() const;
    // save() only if records changed since the last load/save. Thread-safe.
    [[nodiscard]] Result<void> save_if_dirty() const;
    [[nodiscard]] bool         dirty() const noexcept;

    // Import (if needed) and cook one source file (absolute, or content-relative). When
    // `out_result` is non-null and the source was (re)imported, the full ImportResult is
    // moved into it (lets the runtime use fresh data without re-reading cooked files).
    // Thread-safe; serialised per source.
    [[nodiscard]] ImportOutcome import_source(const std::filesystem::path& source,
                                              bool                         force = false,
                                              ImportResult*                out_result = nullptr);

    // Imports every supported source under the content root. Thread-safe, but with
    // `parallel` it must not be called from inside a job (it waits on the JobSystem).
    [[nodiscard]] ScanReport scan(const ScanOptions& options = {});

    // Drops a source's records and deletes its cooked files. Thread-safe.
    bool remove_source(StringView source_path);

    // True when the source file and all recorded dependencies still have the stamps
    // recorded at import time (cheap; no hashing). False for unknown sources.
    [[nodiscard]] bool stamps_match(StringView source_path) const;

    // ---- queries (thread-safe; return copies) ------------------------------------
    [[nodiscard]] std::optional<AssetRecord>  find(AssetId id) const;
    [[nodiscard]] std::optional<AssetRecord>  find(StringView source_path, StringView sub_key) const;
    [[nodiscard]] std::optional<SourceRecord> find_source(StringView source_path) const;
    [[nodiscard]] std::vector<AssetRecord>    assets_of_source(StringView source_path) const;
    [[nodiscard]] AssetId                     primary_asset(StringView source_path) const;
    [[nodiscard]] std::vector<AssetRecord>    all_assets() const;   // sorted by id
    [[nodiscard]] std::vector<String>         all_sources() const;  // sorted
    [[nodiscard]] usize                       asset_count() const;

    // ---- paths (thread-safe) --------------------------------------------------------
    [[nodiscard]] const std::filesystem::path& content_root() const noexcept;
    [[nodiscard]] const std::filesystem::path& cooked_root() const noexcept;
    [[nodiscard]] std::filesystem::path        db_file() const;
    [[nodiscard]] std::filesystem::path        cooked_file(const AssetRecord& record) const;
    [[nodiscard]] std::filesystem::path        source_file(StringView source_path) const;
    // Canonical content-relative path for a file (see canonical_source_path()).
    [[nodiscard]] String                       to_source_path(const std::filesystem::path& file) const;
    [[nodiscard]] const ImportSettings&        import_settings() const noexcept;

    // Content hash used for incremental decisions: FNV-1a over the import-settings
    // fingerprint, the source bytes and each dependency (path + bytes). Thread-safe.
    [[nodiscard]] Result<u64> compute_source_hash(StringView               source_path,
                                                  const std::vector<String>& dependencies) const;

private:
    ImportOutcome import_source_impl(const std::filesystem::path& source, bool force, ImportResult* out_result,
                                     bool import_references);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Stamp of a file on disk (nullopt if it does not exist). Thread-safe.
[[nodiscard]] std::optional<FileStamp> file_stamp(const std::filesystem::path& path);

} // namespace aether::assets
