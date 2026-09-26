// aether/assets/asset_manager.h — runtime asset loading, lifetime and hot reload.
//
// AssetManager hands out AssetRef<T> (shared, refcounted) for asset DATA types
// (asset_types.h). Loads run asynchronously on aether::JobSystem:
//
//   load<T>(id)           -> AssetRef<T> in state Loading; a job resolves the id in the
//                            AssetDatabase and reads the cooked .aeasset when present and up
//                            to date, else (Editor mode) imports + cooks the source first.
//   ref.ready() / get()   -> poll from the main thread; get() is nullptr until Ready.
//   wait_all()            -> block (helping the job system) until every load finished.
//   poll_file_changes()   -> Editor mode hot reload: detects changed source files, re-imports
//                            them on jobs, then (on a later call) swaps the new data in on the
//                            main thread, bumps each asset's version() and fires on_reloaded.
//
// Pointer validity: `const T*` from AssetRef::get() stays valid until the next main-thread
// call to poll_file_changes(), unload() or collect_unused() that replaces/frees that asset.
// Code on other threads must use AssetRef::get_shared(), which pins the current data.
//
// Thread-affinity is documented per function. "Main thread" means the thread that called
// initialize(); reload swaps and callbacks only ever happen there.
#pragma once

#include "aether/assets/asset_database.h"
#include "aether/assets/asset_traits.h"
#include "aether/assets/asset_types.h"
#include "aether/assets/importers.h"
#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>

namespace aether::assets {

enum class AssetState : u8 { Unloaded = 0, Loading, Ready, Failed };

enum class AssetLoadMode : u8 {
    Editor = 0, // cooked data when up to date, else import + cook from source; hot reload
    Runtime,    // cooked data only (shipping); no source access, no hot reload
};

struct AssetManagerConfig {
    std::filesystem::path content_root;  // empty => aether::paths::content_dir()
    std::filesystem::path cooked_root;   // empty => aether::paths::asset_dir()
    AssetLoadMode         mode = AssetLoadMode::Editor;
    bool                  scan_on_startup = false; // Editor: cook the whole content dir in initialize()
    bool                  use_job_system = true;   // false => load() completes synchronously
    f64                   hot_reload_poll_interval = 0.25; // seconds between file-stamp sweeps
    ImportSettings        import_settings;         // content_root is overwritten by the above
};

namespace detail {
// Shared control block behind every AssetRef (one per loaded AssetId). Manager-internal:
// only AssetRef's inline accessors read it directly.
struct AssetEntry {
    AssetId   id;
    AssetType type = AssetType::Unknown;

    std::atomic<AssetState> state{ AssetState::Unloaded };
    std::atomic<u32>        version{ 0 };      // 0 until first Ready; +1 per (re)load
    std::atomic<const void*> raw{ nullptr };   // == data.get(); published with release order

    mutable std::mutex          mutex;         // guards everything below
    std::shared_ptr<const void> data;
    Error                       error;
    u64                         ticket = 0;    // invalidates in-flight loads on unload
};
} // namespace detail

// A shared reference to an asset's data. Cheap to copy; keeps the asset registered (see
// AssetManager::collect_unused). Default-constructed refs are invalid (valid() == false).
template <typename T>
class AssetRef {
public:
    AssetRef() = default;

    // Thread-safe.
    [[nodiscard]] bool      valid() const noexcept { return entry_ != nullptr; }
    [[nodiscard]] AssetId   id() const noexcept { return entry_ ? entry_->id : AssetId{}; }
    [[nodiscard]] AssetState state() const noexcept {
        return entry_ ? entry_->state.load(std::memory_order_acquire) : AssetState::Unloaded;
    }
    [[nodiscard]] bool ready() const noexcept { return state() == AssetState::Ready; }
    [[nodiscard]] bool loading() const noexcept { return state() == AssetState::Loading; }
    [[nodiscard]] bool failed() const noexcept { return state() == AssetState::Failed; }
    // Incremented on every successful (re)load; 0 before the first. Thread-safe.
    [[nodiscard]] u32 version() const noexcept {
        return entry_ ? entry_->version.load(std::memory_order_acquire) : 0u;
    }

    // Main thread only (see the pointer-validity rule in the file comment).
    [[nodiscard]] const T* get() const noexcept {
        if (!ready()) return nullptr;
        return static_cast<const T*>(entry_->raw.load(std::memory_order_acquire));
    }
    [[nodiscard]] const T* operator->() const noexcept { return get(); }

    // Thread-safe: pins the current data (survives reloads/unloads while held).
    [[nodiscard]] std::shared_ptr<const T> get_shared() const {
        if (!entry_) return {};
        std::lock_guard lock(entry_->mutex);
        if (entry_->state.load(std::memory_order_acquire) != AssetState::Ready) return {};
        return std::static_pointer_cast<const T>(entry_->data);
    }

    // The failure reason when failed(). Thread-safe (returns a copy).
    [[nodiscard]] Error error() const {
        if (!entry_) return Error{ ErrorCode::NotInitialized, "invalid AssetRef" };
        std::lock_guard lock(entry_->mutex);
        return entry_->error;
    }

    friend bool operator==(const AssetRef& a, const AssetRef& b) noexcept { return a.entry_ == b.entry_; }

private:
    friend class AssetManager;
    explicit AssetRef(std::shared_ptr<detail::AssetEntry> entry) : entry_(std::move(entry)) {}
    std::shared_ptr<detail::AssetEntry> entry_;
};

// Blueprint §4.2 name for the same concept.
template <typename T>
using AssetHandle = AssetRef<T>;

using ReloadCallback = std::function<void(AssetId)>;
using ListenerId = u64;

class AssetManager {
public:
    AssetManager();
    ~AssetManager(); // calls shutdown()
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Main thread. Opens the asset database (<cooked_root>/asset_db.json); in Editor mode
    // with scan_on_startup, imports/cooks the whole content root first (uses the
    // JobSystem when use_job_system is set - it must be initialised).
    [[nodiscard]] Result<void> initialize(const AssetManagerConfig& config = {});
    // Main thread. Waits for in-flight jobs, saves the database if dirty, drops all assets.
    void shutdown();
    [[nodiscard]] bool is_initialized() const noexcept;

    // Starts an asynchronous load (no-op if already Loading/Ready; retries if Failed or
    // Unloaded) and returns the shared reference. A type mismatch with an already
    // registered id yields a Failed ref. Thread-safe.
    template <CookableAsset T>
    [[nodiscard]] AssetRef<T> load(AssetId id) {
        return AssetRef<T>(acquire(id, asset_type_of_v<T>, LoadKind::Async));
    }
    // Load by content-relative source path; empty sub_key => the source's primary asset.
    // Thread-safe.
    template <CookableAsset T>
    [[nodiscard]] AssetRef<T> load(StringView source_path, StringView sub_key = {}) {
        return load<T>(resolve(source_path, sub_key));
    }

    // Loads on the calling thread (or waits for an in-flight load) and returns with the
    // ref Ready or Failed. Main thread (it may wait on the JobSystem).
    template <CookableAsset T>
    [[nodiscard]] AssetRef<T> load_sync(AssetId id) {
        return AssetRef<T>(acquire(id, asset_type_of_v<T>, LoadKind::Sync));
    }
    template <CookableAsset T>
    [[nodiscard]] AssetRef<T> load_sync(StringView source_path, StringView sub_key = {}) {
        return load_sync<T>(resolve(source_path, sub_key));
    }

    // Returns the registered ref without starting a load (invalid if unknown). Thread-safe.
    template <CookableAsset T>
    [[nodiscard]] AssetRef<T> find(AssetId id) const {
        return AssetRef<T>(find_entry(id, asset_type_of_v<T>));
    }

    // Main thread. Frees the asset's data now (outstanding refs observe Unloaded; a later
    // load() reloads it). In-flight loads of it are discarded.
    void unload(AssetId id);
    // Main thread. Forgets assets no AssetRef refers to any more; returns how many.
    usize collect_unused();
    // Main thread. Blocks until all queued load/reload jobs have finished.
    void wait_all();

    // Main thread. Applies finished reloads (swap data, bump version, fire on_reloaded) and,
    // at most once per hot_reload_poll_interval, checks source stamps of loaded assets and
    // queues re-imports for changed sources. No-op in Runtime mode.
    void poll_file_changes();

    // Registers a callback fired on the main thread (from poll_file_changes) with the id
    // of every asset whose data was replaced. Thread-safe.
    ListenerId on_reloaded(ReloadCallback callback);
    void       remove_reload_listener(ListenerId listener);

    // Thread-safe queries.
    [[nodiscard]] AssetState state(AssetId id) const;
    [[nodiscard]] usize      registered_count() const;
    // Id of a source's sub-asset (empty sub_key => the source's primary asset). The path is
    // canonicalised against the content root. Editor mode: a source unknown to the database
    // is imported synchronously first. Returns an invalid id on failure. Thread-safe.
    [[nodiscard]] AssetId    resolve(StringView source_path, StringView sub_key = {}) const;
    [[nodiscard]] AssetLoadMode mode() const noexcept;

    // The underlying database (thread-safe object; see asset_database.h).
    [[nodiscard]] AssetDatabase&       database() noexcept;
    [[nodiscard]] const AssetDatabase& database() const noexcept;

private:
    enum class LoadKind : u8 { Async, Sync };
    std::shared_ptr<detail::AssetEntry> acquire(AssetId id, AssetType type, LoadKind kind);
    std::shared_ptr<detail::AssetEntry> find_entry(AssetId id, AssetType type) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::assets
