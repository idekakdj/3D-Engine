// aether/scripting/script_vm.h — the per-World Lua 5.4 virtual machine.
//
// One ScriptVM serves one World. It owns a sandboxed Lua state (sol2 is a private
// implementation detail — see lua_integration.h for the opt-in escape hatch), the script
// modules loaded from content/scripts, and one script INSTANCE per entity with a
// ScriptComponent. The full script-side API is documented in content/scripts/README.md.
//
// Guarantees
//   * Sandbox: scripts see base (minus dofile/loadfile, text-only load), math, string (no
//     dump), table, coroutine, utf8, os.clock/os.time and the engine API — never io, os.*,
//     package or debug. `require` only resolves modules under the script root.
//   * Isolation: every call into Lua is protected. A failing instance is disabled and logged
//     ONCE (script path + line + traceback); other instances keep running. The engine never
//     crashes and the log is never spammed.
//   * Runaway protection: each top-level call has an instruction budget (lua_sethook count);
//     a script that exceeds it is killed (even if it wraps the loop in pcall).
//   * Hot reload: changed files are recompiled; instances keep their `self` tables and get
//     on_reload(self). A file that fails to compile keeps the previous version running.
//
// Lifecycle per instance: on_start(self) before the first update or fixed update, then
// on_update(self, dt) / on_fixed_update(self, dt) (alias on_fixed), on_reload(self) after a
// hot reload, on_destroy(self) when the component/entity goes away (only if on_start ran).
//
// Lifetime: the VM must be destroyed before its World. Thread-affinity: main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"
#include "aether/scripting/components.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aether {
class World;
struct InputState;
} // namespace aether

namespace aether::scripting {

struct ScriptVMConfig {
    // Root for script paths and `require`. Empty => paths::content_dir() / "scripts".
    std::filesystem::path script_root;
    // VM instructions allowed per top-level call (lifecycle callback, timer, event dispatch,
    // script load). 0 = unlimited. Checked every 1000 instructions.
    u64 instruction_budget = 10'000'000;
    // Upper bound for Lua heap usage while scripts run, in bytes. 0 = unlimited.
    usize memory_limit = usize{ 256 } << 20;
    // Follow ScriptComponent construction/destruction/replacement through registry signals
    // (instances are created/destroyed at the next update). Off: call attach()/detach().
    bool auto_attach = true;
    // Poll script file timestamps from update() and hot reload changed files.
    bool hot_reload = true;
    f64  hot_reload_interval = 0.5; // seconds of real time between polls
};

enum class ScriptInstanceState : u8 {
    None = 0, // no instance for this entity
    Pending,  // attached; on_start has not run yet
    Running,  // on_start ran; receiving updates
    Inactive, // ScriptComponent::enabled == false
    Failed,   // disabled by a script error (revived when its script is reloaded successfully)
};

class ScriptVM {
public:
    explicit ScriptVM(World& world, ScriptVMConfig config = {});
    ~ScriptVM();

    ScriptVM(const ScriptVM&)            = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;
    ScriptVM(ScriptVM&&)                 = delete;
    ScriptVM& operator=(ScriptVM&&)      = delete;

    [[nodiscard]] World&                       world() const noexcept;
    [[nodiscard]] const ScriptVMConfig&        config() const noexcept;
    [[nodiscard]] const std::filesystem::path& script_root() const noexcept;

    // ---- frame driving -----------------------------------------------------------------------
    // Applies queued attach/detach, polls hot reload, runs on_start/on_update for every
    // enabled instance (attach order), fires due timers, then applies deferred destroys.
    void update(f32 dt);
    // Runs on_start (if needed) and on_fixed_update for every enabled instance.
    void fixed_update(f32 fixed_dt);
    // Input snapshot read by the `input` API (null => no input). Must outlive its use.
    void set_input(const InputState* input) noexcept;

    // ---- instances ---------------------------------------------------------------------------
    // Creates (or re-creates, if the script changed) the instance for `entity`'s
    // ScriptComponent. Loads the script on first use. No Lua code runs here: on_start runs at
    // the next update/fixed_update. Errors are also logged; a script that fails to load leaves
    // the instance in the Failed state until the file is fixed.
    Result<void> attach(Entity entity);
    // Destroys the instance (calls on_destroy(self) if on_start ran). No-op without instance.
    void detach(Entity entity);
    void detach_all();

    [[nodiscard]] ScriptInstanceState instance_state(Entity entity) const;
    [[nodiscard]] std::string         instance_error(Entity entity) const; // "" unless Failed
    [[nodiscard]] usize               instance_count() const noexcept;

    // Reads/writes a field of the instance's `self` table (editor inspector, tests). Values
    // that are not representable as ScriptValue (tables, functions) read as nullopt.
    [[nodiscard]] std::optional<ScriptValue> get_field(Entity entity, std::string_view key);
    Result<void> set_field(Entity entity, std::string_view key, const ScriptValue& value);

    // ---- scripts / hot reload ----------------------------------------------------------------
    // The `properties` defaults a script declares (loads the script if needed). Entries whose
    // values are not representable as ScriptValue are skipped.
    Result<std::vector<ScriptProperty>> declared_properties(std::string_view script_path);
    // Checks every loaded file now (ignores the poll interval). Returns the number of files
    // successfully reloaded (component scripts reloaded + required modules invalidated).
    usize reload_changed();
    // Forces a reload of one component script (path as in ScriptComponent::script).
    Result<void> reload(std::string_view script_path);

    // ---- ad-hoc execution (console, tools, tests) --------------------------------------------
    // Runs `code` in the persistent sandboxed console environment.
    Result<void> run_string(std::string_view code, std::string_view chunk_name = "console");
    // Evaluates `return <expression>` in the console environment.
    Result<ScriptValue> evaluate(std::string_view expression);
    // A global of the console environment (nullopt if not representable).
    [[nodiscard]] std::optional<ScriptValue> get_global(std::string_view name);

    // ---- events ------------------------------------------------------------------------------
    // Emits `name` to every script subscriber (events.subscribe) with a payload table built
    // from `payload`. Returns the number of handlers that ran successfully.
    usize emit_event(std::string_view name, std::span<const ScriptProperty> payload = {});

    // ---- diagnostics -------------------------------------------------------------------------
    [[nodiscard]] usize memory_used() const noexcept;  // bytes on the Lua heap
    [[nodiscard]] u64   error_count() const noexcept;  // script errors reported so far
    [[nodiscard]] f64   script_time() const noexcept;  // accumulated update dt (time.time)
    [[nodiscard]] u64   frame_count() const noexcept;  // update() calls (time.frame)

    // Opaque implementation (sol2). Reachable only through lua_integration.h.
    struct Impl;
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::scripting
