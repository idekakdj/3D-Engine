// asset_manager.cpp — async loading, lifetime and hot reload of asset data.
#include "aether/assets/asset_manager.h"

#include "aether/assets/format.h"
#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/core/time.h"

#include "file_util.h"

#include <algorithm>
#include <format>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace aether::assets {

using detail::AssetEntry;
using EntryPtr = std::shared_ptr<AssetEntry>;
using DataPtr = std::shared_ptr<const void>;

struct AssetManager::Impl {
    AssetManagerConfig config;
    AssetDatabase      db;
    bool               initialized = false;
    std::thread::id    main_thread;
    JobCounter         counter; // every load/reload job

    mutable std::mutex                     entries_mutex;
    std::unordered_map<AssetId, EntryPtr> entries;

    // ---- hot reload ----
    struct Watch {
        std::unordered_set<AssetId> assets;
        bool                        in_flight = false;
        FileStamp                   failed_stamp{ -1, 0 }; // don't retry a broken file until it changes
    };
    struct PendingReload {
        EntryPtr entry;
        u64      ticket = 0;
        DataPtr  data;
    };
    std::mutex                   watch_mutex; // guards watches, completed, finished_sources
    std::map<String, Watch>      watches;     // by source path
    std::vector<PendingReload>   completed;
    std::vector<std::pair<String, FileStamp>> finished_sources; // stamp != {-1,0} => failed at stamp
    f64                          last_poll = -1.0e30;

    std::mutex                                         listeners_mutex;
    std::vector<std::pair<ListenerId, ReloadCallback>> listeners;
    ListenerId                                         next_listener = 1;

    [[nodiscard]] bool editor() const noexcept { return config.mode == AssetLoadMode::Editor; }

    void dispatch(std::function<void()> job) {
        if (config.use_job_system) {
            JobSystem::run(std::move(job), &counter);
        } else {
            job();
        }
    }

    // ------------------------------------------------------------------ loading
    template <typename T>
    static DataPtr take_from(ImportResult& res, AssetId id) {
        for (auto& a : res.list_for<T>()) {
            if (a.id == id) return std::make_shared<const T>(std::move(a.data));
        }
        return {};
    }

    static DataPtr take_from(ImportResult& res, AssetId id, AssetType type) {
        switch (type) {
        case AssetType::Mesh: return take_from<MeshData>(res, id);
        case AssetType::Texture: return take_from<TextureData>(res, id);
        case AssetType::Material: return take_from<MaterialData>(res, id);
        case AssetType::Skeleton: return take_from<SkeletonData>(res, id);
        case AssetType::AnimationClip: return take_from<AnimationClipData>(res, id);
        case AssetType::Scene: return take_from<SceneData>(res, id);
        default: return {};
        }
    }

    template <typename T>
    Result<DataPtr> read_cooked_typed(const AssetRecord& rec) {
        CookedHeader header;
        auto         data = read_asset<T>(db.cooked_file(rec), &header);
        if (!data) return data.error();
        if (header.id != rec.id) {
            return Error{ ErrorCode::IoError, std::format("cooked file {} holds a different asset", rec.cooked_path) };
        }
        if (editor() && (header.source_hash != rec.source_hash || header.importer_version != kImporterVersion)) {
            return Error{ ErrorCode::OutOfDate, std::format("cooked file {} is stale", rec.cooked_path) };
        }
        if (!editor() && header.source_hash != rec.source_hash) {
            AE_LOG_WARN("Assets", "{}: cooked data and asset_db.json disagree on the source hash", rec.cooked_path);
        }
        return DataPtr(std::make_shared<const T>(std::move(*data)));
    }

    Result<DataPtr> read_cooked(const AssetRecord& rec) {
        switch (rec.type) {
        case AssetType::Mesh: return read_cooked_typed<MeshData>(rec);
        case AssetType::Texture: return read_cooked_typed<TextureData>(rec);
        case AssetType::Material: return read_cooked_typed<MaterialData>(rec);
        case AssetType::Skeleton: return read_cooked_typed<SkeletonData>(rec);
        case AssetType::AnimationClip: return read_cooked_typed<AnimationClipData>(rec);
        case AssetType::Scene: return read_cooked_typed<SceneData>(rec);
        default: return Error{ ErrorCode::Unsupported, "asset type has no loader" };
        }
    }

    // Loads the data of `id` (any thread). Editor mode imports/cooks the source when the
    // cooked data is missing or stale.
    Result<DataPtr> load_data(AssetId id, AssetType type) {
        auto rec = db.find(id);
        if (!rec) {
            return Error{ ErrorCode::NotFound, std::format("asset {} is not in the asset database", asset_id_to_hex(id)) };
        }
        if (rec->type != type) {
            return Error{ ErrorCode::InvalidArgument,
                          std::format("asset {} is a {}, requested {}", asset_id_to_hex(id), asset_type_name(rec->type),
                                      asset_type_name(type)) };
        }
        if (!editor()) return read_cooked(*rec);

        const auto source = db.find_source(rec->source_path);
        const bool needs_import =
            !source || source->importer_version != kImporterVersion || !db.stamps_match(rec->source_path);
        Error import_error;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (needs_import || attempt == 1) {
                ImportResult  res;
                ImportOutcome oc = db.import_source(rec->source_path, /*force=*/attempt == 1, &res);
                if (oc.status == ImportStatus::Imported) {
                    if (DataPtr data = take_from(res, id, type)) return data;
                    return Error{ ErrorCode::NotFound, std::format("{} no longer produces {}", rec->source_path, rec->sub_key) };
                }
                if (oc.status == ImportStatus::Failed) import_error = oc.error;
            }
            auto current = db.find(id);
            if (!current) break;
            auto cooked = read_cooked(*current);
            if (cooked) return cooked;
            if (attempt == 1) return cooked.error();
        }
        if (!import_error.message.empty()) return import_error;
        return Error{ ErrorCode::NotFound, std::format("asset {} vanished during import", asset_id_to_hex(id)) };
    }

    void publish(const EntryPtr& e, u64 ticket, Result<DataPtr> result) {
        String source;
        {
            std::lock_guard lock(e->mutex);
            if (e->ticket != ticket) return; // unloaded meanwhile
            if (result) {
                e->data = std::move(*result);
                e->error = {};
                e->raw.store(e->data.get(), std::memory_order_release);
                e->version.fetch_add(1, std::memory_order_acq_rel);
                e->state.store(AssetState::Ready, std::memory_order_release);
            } else {
                e->error = result.error();
                e->data.reset();
                e->raw.store(nullptr, std::memory_order_release);
                e->state.store(AssetState::Failed, std::memory_order_release);
            }
        }
        if (!result) {
            AE_LOG_ERROR("Assets", "failed to load {} ({}): {}", asset_id_to_hex(e->id), asset_type_name(e->type),
                         result.error().message);
        }
        if (editor()) {
            if (auto rec = db.find(e->id)) {
                std::lock_guard lock(watch_mutex);
                watches[rec->source_path].assets.insert(e->id);
            }
        }
    }

    void run_load(const EntryPtr& e, u64 ticket) { publish(e, ticket, load_data(e->id, e->type)); }

    // Marks `e` Loading and returns the ticket, or 0 if it is already Loading/Ready.
    static u64 begin_load(const EntryPtr& e) {
        std::lock_guard lock(e->mutex);
        const AssetState s = e->state.load(std::memory_order_acquire);
        if (s == AssetState::Loading || s == AssetState::Ready) return 0;
        e->ticket += 1;
        e->error = {};
        e->state.store(AssetState::Loading, std::memory_order_release);
        return e->ticket;
    }

    // ------------------------------------------------------------------ reload
    void run_reload(const String& source) {
        std::vector<EntryPtr> targets;
        {
            std::lock_guard wlock(watch_mutex);
            std::lock_guard elock(entries_mutex);
            if (auto w = watches.find(source); w != watches.end()) {
                for (const AssetId& id : w->second.assets) {
                    if (auto it = entries.find(id); it != entries.end()) targets.push_back(it->second);
                }
            }
        }
        const FileStamp stamp_before = file_stamp(db.source_file(source)).value_or(FileStamp{});
        ImportResult    res;
        ImportOutcome   oc = db.import_source(source, false, &res);

        std::vector<PendingReload> ready;
        if (oc.status == ImportStatus::Imported) {
            AE_LOG_INFO("Assets", "hot reload: re-imported {}", source);
            for (const EntryPtr& e : targets) {
                DataPtr data = take_from(res, e->id, e->type);
                if (!data) {
                    AE_LOG_WARN("Assets", "hot reload: {} no longer produces asset {}", source, asset_id_to_hex(e->id));
                    continue;
                }
                u64 ticket;
                {
                    std::lock_guard lock(e->mutex);
                    ticket = e->ticket;
                }
                ready.push_back({ e, ticket, std::move(data) });
            }
        } else if (oc.status == ImportStatus::Failed) {
            AE_LOG_ERROR("Assets", "hot reload of {} failed: {}", source, oc.error.message);
        }
        std::lock_guard lock(watch_mutex);
        for (PendingReload& r : ready) completed.push_back(std::move(r));
        finished_sources.emplace_back(source, oc.status == ImportStatus::Failed ? stamp_before : FileStamp{ -1, 0 });
    }

    void apply_completed_reloads() {
        std::vector<PendingReload>                reloads;
        std::vector<std::pair<String, FileStamp>> finished;
        {
            std::lock_guard lock(watch_mutex);
            reloads.swap(completed);
            finished.swap(finished_sources);
            for (const auto& [source, failed_stamp] : finished) {
                if (auto w = watches.find(source); w != watches.end()) {
                    w->second.in_flight = false;
                    w->second.failed_stamp = failed_stamp;
                }
            }
        }
        std::vector<AssetId> reloaded;
        for (PendingReload& r : reloads) {
            DataPtr old; // released outside the entry lock
            {
                std::lock_guard lock(r.entry->mutex);
                if (r.entry->ticket != r.ticket) continue; // unloaded / reloaded meanwhile
                old = std::move(r.entry->data);
                r.entry->data = std::move(r.data);
                r.entry->error = {};
                r.entry->raw.store(r.entry->data.get(), std::memory_order_release);
                r.entry->version.fetch_add(1, std::memory_order_acq_rel);
                r.entry->state.store(AssetState::Ready, std::memory_order_release);
            }
            reloaded.push_back(r.entry->id);
        }
        if (reloaded.empty()) return;
        std::vector<std::pair<ListenerId, ReloadCallback>> callbacks;
        {
            std::lock_guard lock(listeners_mutex);
            callbacks = listeners;
        }
        for (const AssetId& id : reloaded) {
            for (const auto& [lid, cb] : callbacks) {
                if (cb) cb(id);
            }
        }
    }

    void assert_main_thread() const {
        AE_ASSERT_MSG(!initialized || std::this_thread::get_id() == main_thread,
                      "AssetManager: main-thread-only function called from another thread");
    }
};

// ===========================================================================
// AssetManager
// ===========================================================================
AssetManager::AssetManager() : impl_(std::make_unique<Impl>()) {}

AssetManager::~AssetManager() { shutdown(); }

Result<void> AssetManager::initialize(const AssetManagerConfig& config) {
    if (impl_->initialized) return Error{ ErrorCode::AlreadyExists, "AssetManager already initialised" };
    Impl& d = *impl_;
    d.config = config;
    if (d.config.content_root.empty()) d.config.content_root = paths::content_dir();
    if (d.config.cooked_root.empty()) d.config.cooked_root = paths::asset_dir();
    if (auto r = d.db.open(d.config.content_root, d.config.cooked_root, d.config.import_settings); !r) return r;
    d.config.import_settings = d.db.import_settings();

    if (d.editor() && d.config.scan_on_startup) {
        ScanOptions opts;
        opts.parallel = d.config.use_job_system;
        const ScanReport report = d.db.scan(opts);
        AE_LOG_INFO("Assets", "content scan: {} sources, {} imported, {} up to date, {} failed", report.sources_found,
                    report.imported, report.up_to_date, report.failed);
        for (const ImportOutcome& oc : report.outcomes) {
            if (oc.status == ImportStatus::Failed) AE_LOG_ERROR("Assets", "{}: {}", oc.source_path, oc.error.message);
        }
        if (auto s = d.db.save_if_dirty(); !s) AE_LOG_WARN("Assets", "cannot save asset database: {}", s.error().message);
    }
    d.main_thread = std::this_thread::get_id();
    d.last_poll = -1.0e30;
    d.initialized = true;
    return {};
}

void AssetManager::shutdown() {
    Impl& d = *impl_;
    if (!d.initialized) return;
    wait_all();
    if (d.editor()) {
        if (auto s = d.db.save_if_dirty(); !s) AE_LOG_WARN("Assets", "cannot save asset database: {}", s.error().message);
    }
    {
        std::lock_guard lock(d.entries_mutex);
        for (auto& [id, e] : d.entries) {
            std::lock_guard elock(e->mutex);
            e->ticket += 1;
            e->data.reset();
            e->raw.store(nullptr, std::memory_order_release);
            e->state.store(AssetState::Unloaded, std::memory_order_release);
        }
        d.entries.clear();
    }
    {
        std::lock_guard lock(d.watch_mutex);
        d.watches.clear();
        d.completed.clear();
        d.finished_sources.clear();
    }
    {
        std::lock_guard lock(d.listeners_mutex);
        d.listeners.clear();
    }
    d.initialized = false;
}

bool AssetManager::is_initialized() const noexcept { return impl_->initialized; }

std::shared_ptr<detail::AssetEntry> AssetManager::acquire(AssetId id, AssetType type, LoadKind kind) {
    Impl& d = *impl_;
    auto  failed_entry = [&](Error error) {
        auto e = std::make_shared<AssetEntry>();
        e->id = id;
        e->type = type;
        e->error = std::move(error);
        e->state.store(AssetState::Failed, std::memory_order_release);
        return e;
    };
    if (!d.initialized) return failed_entry(Error{ ErrorCode::NotInitialized, "AssetManager is not initialised" });
    if (!id.is_valid()) return failed_entry(Error{ ErrorCode::InvalidArgument, "invalid AssetId" });

    EntryPtr entry;
    {
        std::lock_guard lock(d.entries_mutex);
        EntryPtr&       slot = d.entries[id];
        if (!slot) {
            slot = std::make_shared<AssetEntry>();
            slot->id = id;
            slot->type = type;
        }
        entry = slot;
    }
    if (entry->type != type) {
        AE_LOG_ERROR("Assets", "asset {} requested as {} but registered as {}", asset_id_to_hex(id),
                     asset_type_name(type), asset_type_name(entry->type));
        return failed_entry(Error{ ErrorCode::InvalidArgument, "asset type mismatch" });
    }

    if (const u64 ticket = Impl::begin_load(entry); ticket != 0) {
        if (kind == LoadKind::Async) {
            d.dispatch([impl = impl_.get(), entry, ticket] { impl->run_load(entry, ticket); });
        } else {
            d.run_load(entry, ticket);
        }
    } else if (kind == LoadKind::Sync) {
        // Another load is in flight: help the job system until it lands.
        while (entry->state.load(std::memory_order_acquire) == AssetState::Loading) {
            if (d.config.use_job_system) {
                JobSystem::wait(d.counter);
            } else {
                std::this_thread::yield();
            }
        }
    }
    return entry;
}

std::shared_ptr<detail::AssetEntry> AssetManager::find_entry(AssetId id, AssetType type) const {
    std::lock_guard lock(impl_->entries_mutex);
    auto            it = impl_->entries.find(id);
    if (it == impl_->entries.end() || it->second->type != type) return {};
    return it->second;
}

void AssetManager::unload(AssetId id) {
    Impl& d = *impl_;
    d.assert_main_thread();
    EntryPtr e;
    {
        std::lock_guard lock(d.entries_mutex);
        auto            it = d.entries.find(id);
        if (it == d.entries.end()) return;
        e = it->second;
    }
    DataPtr old;
    {
        std::lock_guard lock(e->mutex);
        e->ticket += 1;
        old = std::move(e->data);
        e->raw.store(nullptr, std::memory_order_release);
        e->state.store(AssetState::Unloaded, std::memory_order_release);
    }
}

usize AssetManager::collect_unused() {
    Impl& d = *impl_;
    d.assert_main_thread();
    std::vector<EntryPtr> dropped;
    {
        std::lock_guard lock(d.entries_mutex);
        for (auto it = d.entries.begin(); it != d.entries.end();) {
            if (it->second.use_count() == 1 &&
                it->second->state.load(std::memory_order_acquire) != AssetState::Loading) {
                dropped.push_back(std::move(it->second));
                it = d.entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!dropped.empty()) {
        std::lock_guard lock(d.watch_mutex);
        for (const EntryPtr& e : dropped) {
            for (auto& [source, watch] : d.watches) watch.assets.erase(e->id);
        }
        std::erase_if(d.watches, [](const auto& kv) { return kv.second.assets.empty() && !kv.second.in_flight; });
    }
    return dropped.size();
}

void AssetManager::wait_all() {
    Impl& d = *impl_;
    d.assert_main_thread();
    if (d.config.use_job_system && d.initialized) JobSystem::wait(d.counter);
}

void AssetManager::poll_file_changes() {
    Impl& d = *impl_;
    if (!d.initialized || !d.editor()) return;
    d.assert_main_thread();
    d.apply_completed_reloads();

    const f64 now = now_seconds();
    if (now - d.last_poll < d.config.hot_reload_poll_interval) return;
    d.last_poll = now;

    std::vector<String> changed;
    {
        std::lock_guard lock(d.watch_mutex);
        for (auto& [source, watch] : d.watches) {
            if (watch.in_flight || watch.assets.empty() || d.db.stamps_match(source)) continue;
            if (watch.failed_stamp.mtime != -1 &&
                file_stamp(d.db.source_file(source)).value_or(FileStamp{}) == watch.failed_stamp) {
                continue; // still the broken version
            }
            watch.in_flight = true;
            changed.push_back(source);
        }
    }
    for (const String& source : changed) {
        d.dispatch([impl = impl_.get(), source] { impl->run_reload(source); });
    }
    if (!d.config.use_job_system && !changed.empty()) d.apply_completed_reloads();
}

ListenerId AssetManager::on_reloaded(ReloadCallback callback) {
    std::lock_guard lock(impl_->listeners_mutex);
    const ListenerId id = impl_->next_listener++;
    impl_->listeners.emplace_back(id, std::move(callback));
    return id;
}

void AssetManager::remove_reload_listener(ListenerId listener) {
    std::lock_guard lock(impl_->listeners_mutex);
    std::erase_if(impl_->listeners, [&](const auto& l) { return l.first == listener; });
}

AssetState AssetManager::state(AssetId id) const {
    std::lock_guard lock(impl_->entries_mutex);
    auto            it = impl_->entries.find(id);
    return it == impl_->entries.end() ? AssetState::Unloaded : it->second->state.load(std::memory_order_acquire);
}

usize AssetManager::registered_count() const {
    std::lock_guard lock(impl_->entries_mutex);
    return impl_->entries.size();
}

AssetId AssetManager::resolve(StringView source_path, StringView sub_key) const {
    Impl& d = *impl_; // the database is internally synchronised
    if (!d.initialized) return {};
    const String canonical = d.db.to_source_path(d.db.source_file(source_path));
    if (d.editor() && !d.db.find_source(canonical)) {
        // Unknown to the database: import it now so its ids resolve (Editor mode only).
        const ImportOutcome oc = d.db.import_source(d.db.source_file(canonical));
        if (oc.status == ImportStatus::Failed) {
            AE_LOG_ERROR("Assets", "cannot import {}: {}", canonical, oc.error.message);
            return {};
        }
    }
    if (sub_key.empty()) return d.db.primary_asset(canonical);
    return make_asset_id(canonical, sub_key);
}

AssetLoadMode AssetManager::mode() const noexcept { return impl_->config.mode; }

AssetDatabase&       AssetManager::database() noexcept { return impl_->db; }
const AssetDatabase& AssetManager::database() const noexcept { return impl_->db; }

} // namespace aether::assets
