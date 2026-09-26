// vm_impl.h — PRIVATE: ScriptVM::Impl (sol2 state, instances, modules, timers, hot reload).
// Included only by the two sol2 translation units (script_vm.cpp, bindings.cpp).
#pragma once

#include "aether/core/input.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"
#include "aether/scripting/components.h"
#include "aether/scripting/script_vm.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aether::scripting {

// ---- userdata types exposed to Lua (see bindings.cpp) ------------------------------------------
struct LuaEntity {
    Entity id = kNullEntity;
};
struct LightRef {
    Entity id = kNullEntity;
};
struct CameraRef {
    Entity id = kNullEntity;
};
struct TimerHandle {
    u32 id = 0;
};

// Script-visible clock (bound read-only as the `time` global).
struct TimeState {
    f64 time       = 0.0;  // accumulated update dt
    f64 fixed_time = 0.0;  // accumulated fixed dt
    f32 dt         = 0.0f; // dt of the running callback (update or fixed)
    f32 frame_dt   = 0.0f; // dt of the last update()
    f32 fixed_dt   = 1.0f / 60.0f;
    u64 frame      = 0;
};

struct FileStamp {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t                  size   = 0;
    u64                             hash   = 0;
    bool                            exists = false;
};

// A component script (one per path). Versions are swapped in place on hot reload.
struct ScriptModule {
    std::string  path;             // normalized, relative to the script root
    bool         attempted = false; // a load was attempted
    bool         loaded    = false; // a version is live
    std::string  error;            // last load/reload error
    u32          owner     = 0;    // owner id of the live version (its load-time registrations)
    sol::table   env;              // live version environment
    sol::table   table;            // module table (returned table, or env)
    sol::table   instance_mt;      // shared by all instances; stable across reloads
    sol::table   properties;       // declared defaults (invalid if none)
    sol::function on_start, on_update, on_fixed_update, on_destroy, on_reload;
};

struct Instance {
    u32           id     = 0;
    Entity        entity = kNullEntity;
    std::string   raw_path; // ScriptComponent::script as attached (change detection)
    ScriptModule* module = nullptr;
    sol::table    self;
    bool          started = false;
    bool          failed  = false;
    bool          dead    = false;
    std::string   error;
};

struct Timer {
    u32           id       = 0;
    u32           owner    = 0;
    f64           due      = 0.0;
    f64           interval = 0.0;
    bool          repeat   = false;
    bool          cancelled = false;
    sol::main_object fn; // main-thread reference: may outlive the coroutine that created it
};

// Thrown ONLY inside sol2-bound functions (see raise_script_error); sol2's trampoline converts it
// into a Lua error before it can reach any Lua C frame or engine code.
struct ScriptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct AllocState {
    usize used  = 0;
    usize limit = 0;
    bool  armed = false; // the limit applies only while scripts run
};

struct ScriptVM::Impl {
    Impl(ScriptVM& owner, World& world, ScriptVMConfig config);
    ~Impl();

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    static Impl& from(lua_State* L) { return **static_cast<Impl**>(lua_getextraspace(L)); }
    [[nodiscard]] lua_State* state() const { return lua.lua_state(); }

    // ---- protected calls ----------------------------------------------------------------------
    // Stack in: [..., fn, args...]. Out: LUA_OK -> [..., results...]; error -> [..., message].
    // Arms the instruction budget and memory limit for the outermost call.
    // `L` defaults to the main thread; bindings pass the running thread.
    int  pcall(int nargs, int nresults, lua_State* L = nullptr);
    // Pops the error message left by pcall.
    std::string pop_error(lua_State* L = nullptr);
    // Calls fn(args...) protected on the main thread; on failure returns false and fills `error`.
    template <typename Ref, typename... Args>
    bool call(const Ref& fn, std::string& error, Args&&... args);

    // ---- frame driving ------------------------------------------------------------------------
    void update(f32 dt);
    void fixed_update(f32 fixed_dt);

    // ---- owners, failures ---------------------------------------------------------------------
    u32  new_owner_id() { return next_owner_id++; }
    void purge_owner(u32 owner);
    void fail_instance(Instance& inst, std::string_view phase, std::string_view message);
    void owner_failed(u32 owner, std::string_view context, std::string_view message);
    [[nodiscard]] std::string describe(Entity e) const;

    // ---- modules --------------------------------------------------------------------------------
    struct Version {
        sol::table env;
        sol::table table;
        u32        owner = 0;
    };
    bool          load_version(const std::string& path, Version& out, std::string& error);
    void          install_version(ScriptModule& m, Version& v);
    ScriptModule& get_module(const std::string& path);
    bool          reload_module(ScriptModule& m);
    [[nodiscard]] sol::table new_env();
    std::filesystem::path    absolute_path(const std::string& rel) const;
    bool read_script(const std::string& rel, std::string& text, std::string& error);
    void watch(const std::string& rel, const std::string* content);
    // Compiles a module for `require`; leaves the function on the stack on success.
    bool compile_module(lua_State* L, const std::string& key, std::string& error);
    // Read-only proxy of `t` (bootstrap `freeze`).
    sol::table freeze(const sol::table& t);

    // ---- instances ------------------------------------------------------------------------------
    Result<void> attach(Entity e);
    void         destroy_instance(Instance& inst, bool call_on_destroy);
    Instance*    find_instance(Entity e) const;
    Instance*    find_instance_by_id(u32 id) const;
    void         apply_defaults(Instance& inst);
    bool         prepare(Instance& inst); // true => instance may receive a callback now
    void         run_callback(Instance& inst, const sol::function& fn, const char* phase, bool pass_dt,
                              f32 dt);
    [[nodiscard]] ScriptInstanceState state_of(Entity e) const;
    void         flush();
    void         compact();
    void         destroy_entity_now(Entity e);

    // ---- timers ---------------------------------------------------------------------------------
    TimerHandle add_timer(f64 delay, bool repeat, sol::main_object fn);
    void        cancel_timer(u32 id);
    Timer*      find_timer(u32 id);
    void        tick_timers();

    // ---- conversion -----------------------------------------------------------------------------
    sol::object                entity_object(Entity e, lua_State* L = nullptr);
    sol::object                to_lua(const ScriptValue& v, lua_State* L = nullptr);
    static std::optional<ScriptValue> from_lua(const sol::object& o);
    sol::object                copy_value(const sol::object& v, int depth);

    // ---- hot reload -----------------------------------------------------------------------------
    usize reload_changed();

    // ---- registry signals -----------------------------------------------------------------------
    void on_script_constructed(entt::registry&, Entity e) { pending_attach.push_back(e); }
    void on_script_destroyed(entt::registry&, Entity e) { pending_detach.push_back(e); }
    void on_script_updated(entt::registry&, Entity e) {
        pending_detach.push_back(e);
        pending_attach.push_back(e);
    }
    void connect_signals();
    void disconnect_signals();

    void internal_error(std::string_view where, std::string_view what);

    // ---- data (declaration order matters: Lua refs below `lua` die before it) -------------------
    ScriptVM&             vm;
    World&                world;
    ScriptVMConfig        config;
    std::filesystem::path root;
    TimeState             time;
    const InputState*     input = nullptr;
    AllocState            alloc;
    sol::state            lua;

    sol::table    base;           // sandbox globals
    sol::table    env_mt;         // metatable of every script environment
    sol::table    console_env;    // persistent run_string/evaluate environment
    sol::table    loaded_modules; // require cache (key -> value)
    sol::table    entity_cache;   // weak: entity integral -> Entity userdata
    sol::function freeze_fn, make_instance_mt_fn, emit_fn, purge_fn;

    std::unordered_map<std::string, std::unique_ptr<ScriptModule>> modules;
    std::unordered_map<std::string, u32>       module_owner;  // required module key -> owner
    std::unordered_map<std::string, FileStamp> watched;
    std::unordered_map<std::string, std::unordered_set<std::string>> deps; // file -> requires
    std::vector<std::string>                   loading_stack;

    std::vector<std::unique_ptr<Instance>>  instances; // attach order
    std::unordered_map<Entity, Instance*>   by_entity;
    std::vector<Timer>                      timers;
    std::vector<usize>                      due_scratch;
    std::vector<Entity>                     pending_attach, pending_detach, pending_destroy;

    u32  next_owner_id = 1;
    u32  next_timer_id = 1;
    u32  current_owner = 0;
    int  call_depth    = 0;
    bool iterating     = false;
    bool signals_connected = false;
    bool broken        = false;

    u64         budget_used  = 0;
    bool        budget_armed = false;
    std::string budget_where;
    std::string scratch; // message handler buffer

    f64 last_poll   = 0.0;
    u64 error_count = 0;
};

// RAII: sets the current owner (attribution of timers/subscriptions) for a scope.
struct OwnerScope {
    OwnerScope(ScriptVM::Impl& impl, u32 owner) : impl_(impl), prev_(impl.current_owner) {
        impl.current_owner = owner;
    }
    ~OwnerScope() { impl_.current_owner = prev_; }
    OwnerScope(const OwnerScope&)            = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    ScriptVM::Impl& impl_;
    u32             prev_;
};

template <typename Ref, typename... Args>
bool ScriptVM::Impl::call(const Ref& fn, std::string& error, Args&&... args) {
    lua_State* L = state();
    fn.push(L);
    const int nargs = sol::stack::multi_push(L, std::forward<Args>(args)...);
    if (pcall(nargs, 0) != LUA_OK) {
        error = pop_error();
        return false;
    }
    return true;
}

// Registers math, world/entity, component, input, time and timer bindings into impl.base.
void register_bindings(ScriptVM::Impl& impl);

// Canonical entity from a script value (nullopt unless it is an Entity userdata).
std::optional<Entity> entity_from_lua(const sol::object& value);
// Raises "<chunk>:<line>: message" as a Lua error from inside a sol2-bound function.
[[noreturn]] void raise_script_error(lua_State* L, std::string_view message);
// Main-thread copy of a reference (safe to store beyond the calling coroutine).
sol::main_object to_main(const sol::object& o);

// Normalizes a script-relative path ("./ai\\patrol.lua" -> "ai/patrol.lua"). Rejects absolute
// paths, "..", and characters outside [A-Za-z0-9_-. /]. Appends ".lua" when missing.
bool normalize_script_path(std::string_view raw, std::string& out, std::string& error);

} // namespace aether::scripting
