// script_vm.cpp — ScriptVM: sandboxed Lua state, script modules, instances, lifecycle, timers,
// hot reload, and the opt-in lua_integration.h functions. One of the two sol2 TUs.
#include "vm_impl.h"

#include "bootstrap_lua.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/core/time.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/lua_integration.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace aether::scripting {

namespace {

constexpr int kHookGranularity = 1000; // instructions between budget checks
int           kBudgetSentinel  = 0;    // its address is the budget-kill error object

// ---- Lua allocator with a (script-time) heap limit -------------------------------------------
void* lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto*        a   = static_cast<AllocState*>(ud);
    const size_t old = ptr != nullptr ? osize : 0; // for new blocks osize encodes the type
    if (nsize == 0) {
        std::free(ptr);
        a->used -= old;
        return nullptr;
    }
    if (nsize > old && a->armed && a->limit != 0 && a->used + (nsize - old) > a->limit) {
        return nullptr; // Lua runs an emergency GC, then raises "not enough memory"
    }
    void* p = std::realloc(ptr, nsize);
    if (p == nullptr) {
        return nullptr;
    }
    a->used = a->used - old + nsize;
    return p;
}

// ---- instruction budget ------------------------------------------------------------------------
// Runs every kHookGranularity instructions on every thread (coroutines inherit the hook). Raising
// from a count hook is allowed; no C++ object with a destructor is live in this frame.
void budget_hook(lua_State* L, lua_Debug* ar) {
    ScriptVM::Impl& impl = ScriptVM::Impl::from(L);
    if (!impl.budget_armed) {
        return;
    }
    impl.budget_used += kHookGranularity;
    if (impl.budget_used < impl.config.instruction_budget) {
        return;
    }
    if (lua_getinfo(L, "Sl", ar) != 0) {
        impl.budget_where = ar->currentline > 0 ? std::format("{}:{}", ar->short_src, ar->currentline)
                                                : std::string(ar->short_src);
    } else {
        impl.budget_where = "?";
    }
    lua_pushlightuserdata(L, &kBudgetSentinel);
    lua_error(L); // the sandbox's pcall/xpcall/coroutine wrappers re-raise it
}

// Message handler of every protected call: error text + traceback.
int message_handler(lua_State* L) {
    ScriptVM::Impl& impl = ScriptVM::Impl::from(L);
    const char*     msg  = nullptr;
    if (lua_islightuserdata(L, 1) != 0 && lua_touserdata(L, 1) == &kBudgetSentinel) {
        impl.scratch = std::format("instruction budget exceeded ({} instructions) at {}",
                                   impl.config.instruction_budget, impl.budget_where);
        lua_pushlstring(L, impl.scratch.data(), impl.scratch.size());
        msg = lua_tostring(L, -1);
    } else {
        msg = lua_tostring(L, 1);
        if (msg == nullptr) {
            if (luaL_callmeta(L, 1, "__tostring") != 0 && lua_type(L, -1) == LUA_TSTRING) {
                msg = lua_tostring(L, -1);
            } else {
                msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
            }
        }
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

int host_traceback(lua_State* L) {
    const char* msg   = lua_tostring(L, 1);
    const int   level = static_cast<int>(luaL_optinteger(L, 2, 1));
    luaL_traceback(L, L, msg, level);
    return 1;
}

u64 fnv1a(std::string_view s) {
    u64 h = 1469598103934665603ull;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool resolve_module_name(std::string_view name, std::string& key, std::string& error) {
    std::string candidate(name);
    const bool  has_ext = candidate.size() > 4 && candidate.ends_with(".lua");
    if (!has_ext && candidate.find('/') == std::string::npos &&
        candidate.find('\\') == std::string::npos) {
        std::replace(candidate.begin(), candidate.end(), '.', '/'); // "lib.easing" -> lib/easing
    }
    if (!normalize_script_path(candidate, key, error)) {
        error = std::format("module '{}' not found: {}", name, error);
        return false;
    }
    return true;
}

template <typename R, typename F>
R guarded(ScriptVM::Impl& impl, const char* where, R fallback, F&& fn) {
    if (impl.broken) {
        return fallback;
    }
    try {
        return fn();
    } catch (const std::exception& ex) {
        impl.internal_error(where, ex.what());
    } catch (...) {
        impl.internal_error(where, "unknown exception");
    }
    return fallback;
}

template <typename F>
void guarded_void(ScriptVM::Impl& impl, const char* where, F&& fn) {
    if (impl.broken) {
        return;
    }
    try {
        fn();
    } catch (const std::exception& ex) {
        impl.internal_error(where, ex.what());
    } catch (...) {
        impl.internal_error(where, "unknown exception");
    }
}

Result<void> vm_disabled() {
    return make_error(ErrorCode::NotInitialized, "the scripting VM is disabled after an internal error");
}

} // namespace

// =================================================================================================
// free helpers
// =================================================================================================
bool normalize_script_path(std::string_view raw, std::string& out, std::string& error) {
    std::string p(raw);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty()) {
        error = "empty script path";
        return false;
    }
    if (p.front() == '/' || p.find(':') != std::string::npos) {
        error = std::format("script path '{}' must be relative to the script root", raw);
        return false;
    }
    std::string result;
    usize       start = 0;
    while (start <= p.size()) {
        usize end = p.find('/', start);
        if (end == std::string::npos) {
            end = p.size();
        }
        const std::string_view seg(p.data() + start, end - start);
        if (seg == "..") {
            error = std::format("script path '{}' must not leave the script root ('..')", raw);
            return false;
        }
        if (!seg.empty() && seg != ".") {
            for (const char c : seg) {
                const auto uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc) == 0 && c != '_' && c != '-' && c != '.' && c != ' ') {
                    error = std::format("script path '{}' contains an invalid character '{}'", raw, c);
                    return false;
                }
            }
            if (!result.empty()) {
                result += '/';
            }
            result += seg;
        }
        start = end + 1;
    }
    if (result.empty()) {
        error = std::format("script path '{}' is empty", raw);
        return false;
    }
    if (!(result.size() > 4 && result.ends_with(".lua"))) {
        result += ".lua";
    }
    out = std::move(result);
    return true;
}

std::optional<Entity> entity_from_lua(const sol::object& value) {
    if (value.valid() && value.get_type() == sol::type::userdata && value.is<LuaEntity>()) {
        return value.as<LuaEntity>().id;
    }
    return std::nullopt;
}

[[noreturn]] void raise_script_error(lua_State* L, std::string_view message) {
    luaL_where(L, 1);
    const char* where = lua_tostring(L, -1);
    std::string full  = where != nullptr ? where : "";
    lua_pop(L, 1);
    full.append(message);
    throw ScriptError(full);
}

sol::main_object to_main(const sol::object& o) {
    lua_State* L = o.lua_state();
    o.push(L);
    sol::main_object m(L, -1);
    lua_pop(L, 1);
    return m;
}

// =================================================================================================
// Impl: construction / destruction
// =================================================================================================
ScriptVM::Impl::Impl(ScriptVM& owner, World& w, ScriptVMConfig cfg)
    : vm(owner), world(w), config(std::move(cfg)), lua(&sol::default_at_panic, &lua_alloc, &alloc) {
    alloc.limit = config.memory_limit;
    root        = config.script_root.empty() ? paths::content_dir() / "scripts" : config.script_root;
    {
        std::error_code ec;
        auto            canon = std::filesystem::weakly_canonical(root, ec);
        if (!ec) {
            root = std::move(canon);
        }
    }
    try {
        lua_State* L                                   = state();
        *static_cast<Impl**>(lua_getextraspace(L)) = this;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                           sol::lib::coroutine, sol::lib::utf8, sol::lib::os);
        lua_gc(L, LUA_GCGEN, 0, 0); // short-lived math temporaries: generational GC fits best
        if (config.instruction_budget != 0) {
            lua_sethook(L, &budget_hook, LUA_MASKCOUNT, kHookGranularity);
        }

        // ---- host services for the bootstrap -------------------------------------------------
        sol::table host = lua.create_table();
        host.push(L);
        lua_pushlightuserdata(L, &kBudgetSentinel);
        lua_setfield(L, -2, "budget_sentinel");
        lua_pushcfunction(L, &host_traceback);
        lua_setfield(L, -2, "traceback");
        lua_pop(L, 1);

        host.set_function("log", [](sol::this_state s, int level, std::string_view message) {
            lua_State* Ls = s;
            lua_Debug  ar{};
            std::string where = "?";
            if (lua_getstack(Ls, 2, &ar) != 0 && lua_getinfo(Ls, "Sl", &ar) != 0) {
                where = ar.currentline > 0 ? std::format("{}:{}", ar.short_src, ar.currentline)
                                           : std::string(ar.short_src);
            }
            switch (level) {
            case 0: AE_LOG_DEBUG("Script", "[{}] {}", where, message); break;
            case 2: AE_LOG_WARN("Script", "[{}] {}", where, message); break;
            case 3: AE_LOG_ERROR("Script", "[{}] {}", where, message); break;
            default: AE_LOG_INFO("Script", "[{}] {}", where, message); break;
            }
        });
        host.set_function("current_owner", [this]() { return current_owner; });
        host.set_function("set_owner", [this](u32 owner_id) {
            const u32 previous = current_owner;
            current_owner      = owner_id;
            return previous;
        });
        host.set_function("handler_failed",
                          [this](u32 owner_id, std::string_view context, const sol::object& err) {
                              const std::string message = err.get_type() == sol::type::string
                                                              ? err.as<std::string>()
                                                              : std::string("(non-string error)");
                              owner_failed(owner_id, context, message);
                          });
        host.set_function("resolve_module", [](sol::this_state s, std::string_view name) {
            std::string key;
            std::string err;
            if (!resolve_module_name(name, key, err)) {
                return std::make_tuple(sol::make_object(s, sol::lua_nil), sol::make_object(s, err));
            }
            return std::make_tuple(sol::make_object(s, key), sol::make_object(s, sol::lua_nil));
        });
        host.set_function("compile_module", [this](sol::this_state s, const std::string& key) {
            lua_State*  Ls = s;
            std::string err;
            if (!compile_module(Ls, key, err)) {
                return std::make_tuple(sol::make_object(Ls, sol::lua_nil), sol::make_object(Ls, err));
            }
            sol::object fn(Ls, -1);
            lua_pop(Ls, 1);
            if (const auto it = module_owner.find(key); it != module_owner.end()) {
                purge_owner(it->second); // registrations of a previous (failed/invalidated) load
            }
            const u32 owner_id = new_owner_id();
            module_owner[key]  = owner_id;
            loading_stack.push_back(key);
            return std::make_tuple(std::move(fn), sol::make_object(Ls, owner_id));
        });
        host.set_function("note_dependency", [this](const std::string& key) {
            if (!loading_stack.empty() && loading_stack.back() != key) {
                deps[loading_stack.back()].insert(key);
            }
        });
        host.set_function("end_module", [this](const std::string& key) {
            if (!loading_stack.empty() && loading_stack.back() == key) {
                loading_stack.pop_back();
            }
        });

        // ---- bootstrap: builds the sandbox --------------------------------------------------
        const std::string source = std::string(detail::kBootstrapPart1) + detail::kBootstrapPart2;
        if (luaL_loadbufferx(L, source.data(), source.size(), "=[aether]", "t") != LUA_OK) {
            internal_error("bootstrap compile", pop_error());
            return;
        }
        host.push(L);
        if (pcall(1, 2) != LUA_OK) {
            internal_error("bootstrap", pop_error());
            return;
        }
        base                = sol::table(L, -2);
        sol::table internal = sol::table(L, -1);
        lua_pop(L, 2);
        env_mt              = internal.raw_get<sol::table>("env_mt");
        loaded_modules      = internal.raw_get<sol::table>("loaded");
        freeze_fn           = internal.raw_get<sol::function>("freeze");
        make_instance_mt_fn = internal.raw_get<sol::function>("make_instance_mt");
        emit_fn             = internal.raw_get<sol::function>("emit");
        purge_fn            = internal.raw_get<sol::function>("purge_owner");

        entity_cache       = lua.create_table();
        sol::table weak_mt = lua.create_table();
        weak_mt.raw_set("__mode", "v");
        entity_cache[sol::metatable_key] = weak_mt;

        register_bindings(*this);
        console_env = new_env();
    } catch (const std::exception& ex) {
        internal_error("initialization", ex.what());
        return;
    }

    register_script_component_codec(world);
    if (config.auto_attach) {
        connect_signals();
        for (const Entity e : world.view<ScriptComponent>()) {
            pending_attach.push_back(e);
        }
    }
}

ScriptVM::Impl::~Impl() {
    disconnect_signals();
    if (!broken) {
        try {
            for (usize i = instances.size(); i-- > 0;) {
                destroy_instance(*instances[i], true);
            }
        } catch (...) {
            // Destructors never throw; a failure here only skips remaining on_destroy calls.
        }
    }
    // Release every Lua reference while the state is still alive (members declared after `lua`
    // are destroyed before it anyway; clearing explicitly keeps the order obvious).
    timers.clear();
    by_entity.clear();
    instances.clear();
    modules.clear();
}

void ScriptVM::Impl::internal_error(std::string_view where, std::string_view what) {
    broken        = true;
    call_depth    = 0;
    iterating     = false;
    budget_armed  = false;
    alloc.armed   = false;
    ++error_count;
    AE_LOG_ERROR("Script", "internal scripting error during {}: {} -- the ScriptVM is disabled", where,
                 what);
}

void ScriptVM::Impl::connect_signals() {
    if (signals_connected) {
        return;
    }
    auto& reg = world.registry();
    reg.on_construct<ScriptComponent>().connect<&Impl::on_script_constructed>(*this);
    reg.on_destroy<ScriptComponent>().connect<&Impl::on_script_destroyed>(*this);
    reg.on_update<ScriptComponent>().connect<&Impl::on_script_updated>(*this);
    signals_connected = true;
}

void ScriptVM::Impl::disconnect_signals() {
    if (!signals_connected) {
        return;
    }
    auto& reg = world.registry();
    reg.on_construct<ScriptComponent>().disconnect<&Impl::on_script_constructed>(*this);
    reg.on_destroy<ScriptComponent>().disconnect<&Impl::on_script_destroyed>(*this);
    reg.on_update<ScriptComponent>().disconnect<&Impl::on_script_updated>(*this);
    signals_connected = false;
}

// =================================================================================================
// protected calls
// =================================================================================================
int ScriptVM::Impl::pcall(int nargs, int nresults, lua_State* L) {
    if (L == nullptr) {
        L = state();
    }
    const int fn_index = lua_gettop(L) - nargs;
    lua_pushcfunction(L, &message_handler);
    lua_insert(L, fn_index);
    const bool outermost = call_depth == 0;
    if (outermost) {
        budget_used  = 0;
        budget_armed = config.instruction_budget != 0;
        alloc.armed  = true;
    }
    ++call_depth;
    const int status = lua_pcall(L, nargs, nresults, fn_index);
    --call_depth;
    if (outermost) {
        budget_armed = false;
        alloc.armed  = false;
    }
    lua_remove(L, fn_index);
    return status;
}

std::string ScriptVM::Impl::pop_error(lua_State* L) {
    if (L == nullptr) {
        L = state();
    }
    size_t      len = 0;
    const char* s   = lua_tolstring(L, -1, &len);
    std::string out = s != nullptr ? std::string(s, len) : std::string("(non-string error)");
    lua_pop(L, 1);
    return out;
}

sol::table ScriptVM::Impl::freeze(const sol::table& t) {
    lua_State* L = state();
    freeze_fn.push(L);
    t.push(L);
    if (pcall(1, 1) != LUA_OK) {
        internal_error("freeze", pop_error());
        return t;
    }
    sol::table proxy(L, -1);
    lua_pop(L, 1);
    return proxy;
}

sol::table ScriptVM::Impl::new_env() {
    sol::table env            = lua.create_table();
    env[sol::metatable_key]   = env_mt;
    env.raw_set("_G", env);
    return env;
}

// =================================================================================================
// owners & failures
// =================================================================================================
void ScriptVM::Impl::purge_owner(u32 owner) {
    if (owner == 0) {
        return;
    }
    for (Timer& t : timers) {
        if (t.owner == owner) {
            t.cancelled = true;
        }
    }
    if (!purge_fn.valid()) {
        return;
    }
    lua_State* L = state();
    purge_fn.push(L);
    lua_pushinteger(L, static_cast<lua_Integer>(owner));
    if (pcall(1, 0) != LUA_OK) {
        const std::string err = pop_error();
        AE_LOG_ERROR("Script", "internal: purging subscriptions failed: {}", err);
    }
}

std::string ScriptVM::Impl::describe(Entity e) const {
    const auto id = entt::to_integral(e);
    if (!world.valid(e)) {
        return std::format("entity #{} (destroyed)", id);
    }
    const auto* name = world.try_get<NameComponent>(e);
    return std::format("entity '{}' (#{})", name != nullptr ? name->name : std::string(), id);
}

void ScriptVM::Impl::fail_instance(Instance& inst, std::string_view phase, std::string_view message) {
    if (inst.failed || inst.dead) {
        return; // already disabled: never log twice
    }
    inst.failed = true;
    inst.error  = message;
    ++error_count;
    AE_LOG_ERROR("Script",
                 "'{}' on {}: {} failed; the instance is disabled until the script is reloaded.\n{}",
                 inst.module != nullptr ? inst.module->path : inst.raw_path, describe(inst.entity),
                 phase, message);
    purge_owner(inst.id); // its timers and event handlers stop with it
}

void ScriptVM::Impl::owner_failed(u32 owner, std::string_view context, std::string_view message) {
    if (Instance* inst = find_instance_by_id(owner)) {
        fail_instance(*inst, context, message);
        return;
    }
    std::string source = "console";
    for (const auto& [key, id] : module_owner) {
        if (id == owner) {
            source = key;
        }
    }
    for (const auto& [path, m] : modules) {
        if (m->owner == owner) {
            source = path;
        }
    }
    ++error_count;
    AE_LOG_ERROR("Script", "{} registered by '{}' failed and was removed.\n{}", context, source,
                 message);
}

// =================================================================================================
// modules
// =================================================================================================
std::filesystem::path ScriptVM::Impl::absolute_path(const std::string& rel) const {
    return root / std::filesystem::path(rel);
}

bool ScriptVM::Impl::read_script(const std::string& rel, std::string& text, std::string& error) {
    const std::filesystem::path path = absolute_path(rel);
    std::error_code             ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = std::format("script '{}' not found ({})", rel, path.generic_string());
        return false;
    }
    const auto canon = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        const auto relative = canon.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
            error = std::format("script '{}' resolves outside the script root", rel);
            return false;
        }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("script '{}' could not be opened", rel);
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void ScriptVM::Impl::watch(const std::string& rel, const std::string* content) {
    FileStamp                   stamp;
    const std::filesystem::path path = absolute_path(rel);
    std::error_code             ec;
    if (content != nullptr) {
        stamp.exists = true;
        stamp.hash   = fnv1a(*content);
        stamp.mtime  = std::filesystem::last_write_time(path, ec);
        stamp.size   = std::filesystem::file_size(path, ec);
    }
    watched[rel] = stamp;
}

bool ScriptVM::Impl::compile_module(lua_State* L, const std::string& key, std::string& error) {
    std::string text;
    const bool  have = read_script(key, text, error);
    watch(key, have ? &text : nullptr);
    if (!have) {
        return false;
    }
    const std::string chunkname = "@" + key;
    if (luaL_loadbufferx(L, text.data(), text.size(), chunkname.c_str(), "t") != LUA_OK) {
        error = pop_error(L);
        return false;
    }
    sol::table env = new_env();
    env.push(L);
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        lua_pop(L, 1);
    }
    return true;
}

bool ScriptVM::Impl::load_version(const std::string& path, Version& out, std::string& error) {
    lua_State*  L = state();
    std::string text;
    const bool  have = read_script(path, text, error);
    watch(path, have ? &text : nullptr);
    if (!have) {
        return false;
    }
    const std::string chunkname = "@" + path;
    if (luaL_loadbufferx(L, text.data(), text.size(), chunkname.c_str(), "t") != LUA_OK) {
        error = pop_error();
        return false;
    }
    sol::table env = new_env();
    env.push(L);
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        lua_pop(L, 1);
    }

    // Execute the chunk (top-level code: `properties`, function definitions, requires).
    const u32   owner_id  = new_owner_id();
    const usize stack_top = loading_stack.size();
    auto        old_deps  = std::move(deps[path]);
    deps[path].clear();
    loading_stack.push_back(path);
    int status = LUA_OK;
    {
        OwnerScope scope(*this, owner_id);
        status = pcall(0, 1);
    }
    loading_stack.resize(stack_top);
    if (status != LUA_OK) {
        error      = pop_error();
        deps[path] = std::move(old_deps);
        purge_owner(owner_id);
        return false;
    }
    sol::object result(L, -1);
    lua_pop(L, 1);
    out.env   = env;
    out.owner = owner_id;
    out.table = result.get_type() == sol::type::table ? result.as<sol::table>() : env;
    return true;
}

void ScriptVM::Impl::install_version(ScriptModule& m, Version& v) {
    m.env    = std::move(v.env);
    m.table  = std::move(v.table);
    m.owner  = v.owner;
    m.loaded = true;
    m.error.clear();
    m.instance_mt.raw_set("module", m.table);
    const auto fn = [&](const char* name) -> sol::function {
        sol::object o = m.table.raw_get<sol::object>(name);
        return o.get_type() == sol::type::function ? o.as<sol::function>() : sol::function();
    };
    m.on_start        = fn("on_start");
    m.on_update       = fn("on_update");
    m.on_fixed_update = fn("on_fixed_update");
    if (!m.on_fixed_update.valid()) {
        m.on_fixed_update = fn("on_fixed"); // blueprint §11.1 spelling
    }
    m.on_destroy = fn("on_destroy");
    m.on_reload  = fn("on_reload");
    const sol::object props = m.table.raw_get<sol::object>("properties");
    m.properties = props.get_type() == sol::type::table ? props.as<sol::table>() : sol::table();
}

ScriptModule& ScriptVM::Impl::get_module(const std::string& path) {
    if (const auto it = modules.find(path); it != modules.end()) {
        return *it->second;
    }
    auto m  = std::make_unique<ScriptModule>();
    m->path = path;
    {
        lua_State* L = state();
        make_instance_mt_fn.push(L);
        lua_newtable(L);
        if (pcall(1, 1) == LUA_OK) {
            m->instance_mt = sol::table(L, -1);
            lua_pop(L, 1);
        } else {
            internal_error("instance metatable", pop_error());
            m->instance_mt = lua.create_table();
        }
    }
    ScriptModule& ref = *m;
    modules.emplace(path, std::move(m));

    ref.attempted = true;
    Version     v;
    std::string err;
    if (load_version(path, v, err)) {
        install_version(ref, v);
    } else {
        ref.error = err;
        ++error_count;
        AE_LOG_ERROR("Script", "failed to load '{}':\n{}", path, err);
    }
    return ref;
}

bool ScriptVM::Impl::reload_module(ScriptModule& m) {
    Version     v;
    std::string err;
    if (!load_version(m.path, v, err)) {
        ++error_count;
        if (m.loaded) {
            AE_LOG_ERROR("Script", "hot reload of '{}' failed; keeping the previous version.\n{}",
                         m.path, err);
        } else {
            m.error = err;
            AE_LOG_ERROR("Script", "reload of '{}' failed.\n{}", m.path, err);
        }
        return false;
    }
    const u32 old_owner = m.owner;
    install_version(m, v);
    purge_owner(old_owner); // load-time registrations of the previous version

    usize count = 0;
    for (usize i = 0; i < instances.size(); ++i) {
        Instance& inst = *instances[i];
        if (inst.dead || inst.module != &m) {
            continue;
        }
        ++count;
        apply_defaults(inst); // properties added by the new version
        if (inst.failed) {
            // Revive: restart with on_start(self); the self table keeps its fields.
            inst.failed  = false;
            inst.started = false;
            inst.error.clear();
            continue;
        }
        if (inst.started && m.on_reload.valid()) {
            run_callback(inst, m.on_reload, "on_reload", false, 0.0f);
        }
    }
    AE_LOG_INFO("Script", "reloaded '{}' ({} instance{})", m.path, count, count == 1 ? "" : "s");
    return true;
}

usize ScriptVM::Impl::reload_changed() {
    std::vector<std::string> changed;
    for (auto& [path, stamp] : watched) {
        const std::filesystem::path abs = absolute_path(path);
        std::error_code             ec;
        if (!std::filesystem::is_regular_file(abs, ec)) {
            if (stamp.exists) {
                stamp.exists = false;
                AE_LOG_WARN("Script", "'{}' was deleted; the loaded version keeps running", path);
            }
            continue;
        }
        const auto mtime = std::filesystem::last_write_time(abs, ec);
        if (ec) {
            continue;
        }
        const auto size = std::filesystem::file_size(abs, ec);
        if (ec) {
            continue;
        }
        if (stamp.exists && mtime == stamp.mtime && size == stamp.size) {
            continue;
        }
        std::string text;
        std::string err;
        if (!read_script(path, text, err)) {
            continue;
        }
        const u64  h               = fnv1a(text);
        const bool content_changed = !stamp.exists || h != stamp.hash;
        stamp.mtime                = mtime;
        stamp.size                 = size;
        stamp.hash                 = h;
        stamp.exists               = true;
        if (content_changed) {
            changed.push_back(path);
        }
    }
    if (changed.empty()) {
        return 0;
    }

    // Everything that (transitively) required a changed file is affected too.
    std::unordered_set<std::string> affected(changed.begin(), changed.end());
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [file, requires_set] : deps) {
            if (affected.contains(file)) {
                continue;
            }
            for (const std::string& r : requires_set) {
                if (affected.contains(r)) {
                    affected.insert(file);
                    grew = true;
                    break;
                }
            }
        }
    }
    std::vector<std::string> ordered(affected.begin(), affected.end());
    std::sort(ordered.begin(), ordered.end());

    usize count = 0;
    for (const std::string& key : ordered) {
        if (const auto it = module_owner.find(key); it != module_owner.end()) {
            purge_owner(it->second);
            module_owner.erase(it);
            loaded_modules.raw_set(key, sol::lua_nil);
            if (std::find(changed.begin(), changed.end(), key) != changed.end()) {
                ++count;
                AE_LOG_INFO("Script", "module '{}' changed; it is re-required on next use", key);
            }
        }
    }
    for (const std::string& key : ordered) {
        if (const auto it = modules.find(key); it != modules.end()) {
            if (reload_module(*it->second)) {
                ++count;
            }
        }
    }
    return count;
}

// =================================================================================================
// instances
// =================================================================================================
Instance* ScriptVM::Impl::find_instance(Entity e) const {
    const auto it = by_entity.find(e);
    return it != by_entity.end() && !it->second->dead ? it->second : nullptr;
}

Instance* ScriptVM::Impl::find_instance_by_id(u32 id) const {
    for (const auto& inst : instances) {
        if (inst->id == id && !inst->dead) {
            return inst.get();
        }
    }
    return nullptr;
}

sol::object ScriptVM::Impl::copy_value(const sol::object& v, int depth) {
    lua_State* L = state();
    switch (v.get_type()) {
    case sol::type::table: {
        if (depth >= 16) {
            return v;
        }
        const sol::table src = v.as<sol::table>();
        sol::table       dst = lua.create_table();
        src.for_each([&](const sol::object& k, const sol::object& val) {
            dst.raw_set(k, copy_value(val, depth + 1));
        });
        src.push(L);
        if (lua_getmetatable(L, -1) != 0) {
            dst.push(L);
            lua_insert(L, -2);
            lua_setmetatable(L, -2);
            lua_pop(L, 2);
        } else {
            lua_pop(L, 1);
        }
        return dst;
    }
    case sol::type::userdata:
        if (v.is<Vec3>()) {
            return sol::make_object(L, v.as<Vec3>());
        }
        if (v.is<Quat>()) {
            return sol::make_object(L, v.as<Quat>());
        }
        if (v.is<Vec2>()) {
            return sol::make_object(L, v.as<Vec2>());
        }
        if (v.is<Vec4>()) {
            return sol::make_object(L, v.as<Vec4>());
        }
        if (v.is<Transform>()) {
            return sol::make_object(L, v.as<Transform>());
        }
        return v;
    default: return v;
    }
}

void ScriptVM::Impl::apply_defaults(Instance& inst) {
    ScriptModule& m = *inst.module;
    if (!m.loaded || !m.properties.valid()) {
        return;
    }
    m.properties.for_each([&](const sol::object& k, const sol::object& v) {
        if (inst.self.raw_get<sol::object>(k).get_type() == sol::type::lua_nil) {
            inst.self.raw_set(k, copy_value(v, 0));
        }
    });
}

Result<void> ScriptVM::Impl::attach(Entity e) {
    if (!world.valid(e)) {
        return make_error(ErrorCode::InvalidArgument, "attach: invalid entity");
    }
    const ScriptComponent* comp = world.try_get<ScriptComponent>(e);
    if (comp == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "attach: entity has no ScriptComponent");
    }
    if (Instance* existing = find_instance(e)) {
        if (existing->id == comp->runtime_id && existing->raw_path == comp->script) {
            return {};
        }
        destroy_instance(*existing, true); // runs Lua: re-fetch the component below
    }
    auto* c = world.valid(e) ? world.try_get<ScriptComponent>(e) : nullptr;
    if (c == nullptr) {
        return {};
    }
    if (c->script.empty()) {
        c->runtime_id = kInvalidU32;
        return {};
    }
    std::string path;
    std::string err;
    if (!normalize_script_path(c->script, path, err)) {
        ++error_count;
        AE_LOG_ERROR("Script", "{}: {}", describe(e), err);
        return make_error(ErrorCode::InvalidArgument, err);
    }
    ScriptModule& m = get_module(path); // may run the script's top-level code
    c               = world.valid(e) ? world.try_get<ScriptComponent>(e) : nullptr;
    if (c == nullptr) {
        return {};
    }

    auto inst      = std::make_unique<Instance>();
    inst->id       = new_owner_id();
    inst->entity   = e;
    inst->raw_path = c->script;
    inst->module   = &m;
    inst->self     = lua.create_table();
    inst->self.raw_set("entity", entity_object(e));
    inst->self[sol::metatable_key] = m.instance_mt;
    for (const ScriptProperty& p : c->properties) {
        inst->self.raw_set(p.name, to_lua(p.value));
        if (m.loaded && (!m.properties.valid() ||
                         m.properties.raw_get<sol::object>(p.name).get_type() == sol::type::lua_nil)) {
            AE_LOG_WARN("Script", "'{}' on {}: property override '{}' is not declared in `properties`",
                        path, describe(e), p.name);
        }
    }
    if (m.loaded) {
        apply_defaults(*inst);
    } else {
        inst->failed = true; // the load error was logged once for the script
        inst->error  = m.error;
    }
    c->runtime_id = inst->id;
    by_entity[e]  = inst.get();
    instances.push_back(std::move(inst));
    if (!m.loaded) {
        return make_error(ErrorCode::CompilationFailed, m.error);
    }
    return {};
}

void ScriptVM::Impl::destroy_instance(Instance& inst, bool call_on_destroy) {
    if (inst.dead) {
        return;
    }
    inst.dead = true;
    if (const auto it = by_entity.find(inst.entity); it != by_entity.end() && it->second == &inst) {
        by_entity.erase(it);
    }
    if (call_on_destroy && inst.started && !inst.failed && inst.module->on_destroy.valid()) {
        std::string err;
        OwnerScope  scope(*this, inst.id);
        if (!call(inst.module->on_destroy, err, inst.self)) {
            ++error_count;
            AE_LOG_ERROR("Script", "'{}' on {}: on_destroy failed.\n{}", inst.module->path,
                         describe(inst.entity), err);
        }
    }
    purge_owner(inst.id);
    if (world.valid(inst.entity)) {
        if (auto* c = world.try_get<ScriptComponent>(inst.entity); c != nullptr && c->runtime_id == inst.id) {
            c->runtime_id = kInvalidU32;
        }
    }
}

ScriptInstanceState ScriptVM::Impl::state_of(Entity e) const {
    const Instance* inst = find_instance(e);
    if (inst == nullptr) {
        return ScriptInstanceState::None;
    }
    if (inst->failed) {
        return ScriptInstanceState::Failed;
    }
    const auto* c = world.valid(e) ? world.try_get<ScriptComponent>(e) : nullptr;
    if (c != nullptr && !c->enabled) {
        return ScriptInstanceState::Inactive;
    }
    return inst->started ? ScriptInstanceState::Running : ScriptInstanceState::Pending;
}

bool ScriptVM::Impl::prepare(Instance& inst) {
    if (inst.dead || inst.failed) {
        return false;
    }
    const auto* c = world.valid(inst.entity) ? world.try_get<ScriptComponent>(inst.entity) : nullptr;
    if (c == nullptr || c->runtime_id != inst.id || c->script != inst.raw_path) {
        pending_detach.push_back(inst.entity);
        if (c != nullptr) {
            pending_attach.push_back(inst.entity);
        }
        return false;
    }
    if (!c->enabled) {
        return false;
    }
    if (!inst.started) {
        inst.started = true;
        if (inst.module->on_start.valid()) {
            run_callback(inst, inst.module->on_start, "on_start", false, 0.0f);
        }
    }
    return !inst.failed && !inst.dead;
}

void ScriptVM::Impl::run_callback(Instance& inst, const sol::function& fn, const char* phase,
                                  bool pass_dt, f32 dt) {
    std::string err;
    bool        ok = false;
    {
        OwnerScope scope(*this, inst.id);
        ok = pass_dt ? call(fn, err, inst.self, dt) : call(fn, err, inst.self);
    }
    if (!ok) {
        fail_instance(inst, phase, err);
    }
}

void ScriptVM::Impl::destroy_entity_now(Entity e) {
    if (!world.valid(e)) {
        return;
    }
    std::vector<Entity> subtree;
    scene::for_each_descendant(world, e, [&](Entity d) { subtree.push_back(d); });
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) { // children before parents
        if (Instance* inst = find_instance(*it)) {
            destroy_instance(*inst, true);
        }
    }
    if (Instance* inst = find_instance(e)) {
        destroy_instance(*inst, true);
    }
    if (world.valid(e)) {
        world.destroy(e);
    }
}

void ScriptVM::Impl::flush() {
    if (iterating || call_depth > 0) {
        return;
    }
    for (int pass = 0; pass < 16; ++pass) {
        if (pending_destroy.empty() && pending_detach.empty() && pending_attach.empty()) {
            break;
        }
        std::vector<Entity> destroys;
        destroys.swap(pending_destroy);
        for (const Entity e : destroys) {
            destroy_entity_now(e);
        }
        std::vector<Entity> detaches;
        detaches.swap(pending_detach);
        for (const Entity e : detaches) {
            Instance* inst = find_instance(e);
            if (inst == nullptr) {
                continue;
            }
            const auto* c = world.valid(e) ? world.try_get<ScriptComponent>(e) : nullptr;
            if (c == nullptr || c->runtime_id != inst->id || c->script != inst->raw_path) {
                destroy_instance(*inst, true);
            }
        }
        std::vector<Entity> attaches;
        attaches.swap(pending_attach);
        for (const Entity e : attaches) {
            if (world.valid(e) && world.has<ScriptComponent>(e)) {
                (void)attach(e);
            }
        }
    }
    compact();
}

void ScriptVM::Impl::compact() {
    if (iterating || call_depth > 0) {
        return;
    }
    std::erase_if(instances, [](const std::unique_ptr<Instance>& p) { return p->dead; });
    std::erase_if(timers, [](const Timer& t) { return t.cancelled; });
}

// =================================================================================================
// frame driving
// =================================================================================================
void ScriptVM::Impl::update(f32 dt) {
    flush();
    time.dt       = dt;
    time.frame_dt = dt;
    time.time += static_cast<f64>(dt);
    ++time.frame;
    if (config.hot_reload) {
        const f64 now = now_seconds();
        if (now - last_poll >= config.hot_reload_interval) {
            last_poll = now;
            reload_changed();
        }
    }
    iterating     = true;
    const usize n = instances.size();
    for (usize i = 0; i < n; ++i) {
        Instance& inst = *instances[i];
        if (!prepare(inst)) {
            continue;
        }
        if (inst.module->on_update.valid()) {
            run_callback(inst, inst.module->on_update, "on_update", true, dt);
        }
    }
    iterating = false;
    tick_timers();
    flush();
}

void ScriptVM::Impl::fixed_update(f32 fixed_dt) {
    flush();
    time.fixed_dt = fixed_dt;
    time.fixed_time += static_cast<f64>(fixed_dt);
    time.dt       = fixed_dt;
    iterating     = true;
    const usize n = instances.size();
    for (usize i = 0; i < n; ++i) {
        Instance& inst = *instances[i];
        if (!prepare(inst)) {
            continue;
        }
        if (inst.module->on_fixed_update.valid()) {
            run_callback(inst, inst.module->on_fixed_update, "on_fixed_update", true, fixed_dt);
        }
    }
    iterating = false;
    time.dt   = time.frame_dt;
    flush();
}

// =================================================================================================
// timers
// =================================================================================================
TimerHandle ScriptVM::Impl::add_timer(f64 delay, bool repeat, sol::main_object fn) {
    Timer t;
    t.id       = next_timer_id++;
    t.owner    = current_owner;
    t.interval = delay;
    t.repeat   = repeat;
    t.due      = time.time + delay;
    t.fn       = std::move(fn);
    const u32 id = t.id;
    timers.push_back(std::move(t));
    return TimerHandle{ id };
}

Timer* ScriptVM::Impl::find_timer(u32 id) {
    for (Timer& t : timers) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

void ScriptVM::Impl::cancel_timer(u32 id) {
    if (Timer* t = find_timer(id)) {
        t->cancelled = true;
    }
}

void ScriptVM::Impl::tick_timers() {
    if (timers.empty()) {
        return;
    }
    const u32 id_limit = next_timer_id; // timers created by callbacks wait for the next tick
    const f64 now      = time.time;
    due_scratch.clear();
    for (usize i = 0; i < timers.size(); ++i) {
        const Timer& t = timers[i];
        if (!t.cancelled && t.id < id_limit && t.due <= now) {
            due_scratch.push_back(i);
        }
    }
    std::sort(due_scratch.begin(), due_scratch.end(), [this](usize a, usize b) {
        const Timer& ta = timers[a];
        const Timer& tb = timers[b];
        return ta.due != tb.due ? ta.due < tb.due : ta.id < tb.id;
    });
    lua_State* L = state();
    iterating    = true;
    for (const usize idx : due_scratch) {
        if (timers[idx].cancelled) {
            continue; // cancelled by an earlier callback
        }
        const u32 owner_id = timers[idx].owner;
        const u32 id       = timers[idx].id;
        int       status   = LUA_OK;
        {
            OwnerScope scope(*this, owner_id);
            timers[idx].fn.push(L); // callbacks may append timers: never hold a Timer& across
            sol::stack::push(L, TimerHandle{ id });
            status = pcall(1, 0);
        }
        if (status != LUA_OK) {
            const std::string err = pop_error();
            timers[idx].cancelled = true;
            owner_failed(owner_id, "timer callback", err);
            continue;
        }
        Timer& t = timers[idx];
        if (t.cancelled) {
            continue;
        }
        if (t.repeat) {
            t.due += t.interval;
            if (t.due <= now) {
                t.due = now + t.interval; // never fire more than once per tick
            }
        } else {
            t.cancelled = true;
        }
    }
    iterating = false;
}

// =================================================================================================
// conversion
// =================================================================================================
sol::object ScriptVM::Impl::entity_object(Entity e, lua_State* L) {
    if (L == nullptr) {
        L = state();
    }
    if (e == kNullEntity) {
        return sol::make_object(L, sol::lua_nil);
    }
    const auto key = static_cast<lua_Integer>(entt::to_integral(e));
    entity_cache.push(L);
    if (lua_rawgeti(L, -1, key) != LUA_TNIL) {
        sol::object o(L, -1);
        lua_pop(L, 2);
        return o;
    }
    lua_pop(L, 1);
    sol::stack::push(L, LuaEntity{ e });
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, key);
    sol::object o(L, -1);
    lua_pop(L, 2);
    return o;
}

sol::object ScriptVM::Impl::to_lua(const ScriptValue& v, lua_State* L) {
    if (L == nullptr) {
        L = state();
    }
    return std::visit(
        [&](const auto& x) -> sol::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return sol::make_object(L, sol::lua_nil);
            } else if constexpr (std::is_same_v<T, Entity>) {
                return entity_object(x, L);
            } else {
                return sol::make_object(L, x);
            }
        },
        v);
}

std::optional<ScriptValue> ScriptVM::Impl::from_lua(const sol::object& o) {
    if (!o.valid()) {
        return ScriptValue{};
    }
    switch (o.get_type()) {
    case sol::type::lua_nil:
    case sol::type::none: return ScriptValue{};
    case sol::type::boolean: return ScriptValue{ o.as<bool>() };
    case sol::type::number: {
        lua_State* L = o.lua_state();
        o.push(L);
        const ScriptValue v = lua_isinteger(L, -1) != 0
                                  ? ScriptValue{ static_cast<i64>(lua_tointeger(L, -1)) }
                                  : ScriptValue{ static_cast<f64>(lua_tonumber(L, -1)) };
        lua_pop(L, 1);
        return v;
    }
    case sol::type::string: return ScriptValue{ o.as<std::string>() };
    case sol::type::userdata:
        if (o.is<Vec3>()) {
            return ScriptValue{ o.as<Vec3>() };
        }
        if (o.is<Quat>()) {
            return ScriptValue{ o.as<Quat>() };
        }
        if (o.is<Vec2>()) {
            return ScriptValue{ o.as<Vec2>() };
        }
        if (o.is<Vec4>()) {
            return ScriptValue{ o.as<Vec4>() };
        }
        if (o.is<LuaEntity>()) {
            return ScriptValue{ o.as<LuaEntity>().id };
        }
        return std::nullopt;
    default: return std::nullopt;
    }
}

// =================================================================================================
// ScriptVM (public API)
// =================================================================================================
ScriptVM::ScriptVM(World& world, ScriptVMConfig config)
    : impl_(std::make_unique<Impl>(*this, world, std::move(config))) {}

ScriptVM::~ScriptVM() = default;

World&                       ScriptVM::world() const noexcept { return impl_->world; }
const ScriptVMConfig&        ScriptVM::config() const noexcept { return impl_->config; }
const std::filesystem::path& ScriptVM::script_root() const noexcept { return impl_->root; }

void ScriptVM::update(f32 dt) {
    guarded_void(*impl_, "update", [&] { impl_->update(dt); });
}

void ScriptVM::fixed_update(f32 fixed_dt) {
    guarded_void(*impl_, "fixed_update", [&] { impl_->fixed_update(fixed_dt); });
}

void ScriptVM::set_input(const InputState* input) noexcept { impl_->input = input; }

Result<void> ScriptVM::attach(Entity entity) {
    return guarded(*impl_, "attach", vm_disabled(), [&]() -> Result<void> {
        Result<void> r = impl_->attach(entity);
        impl_->compact();
        return r;
    });
}

void ScriptVM::detach(Entity entity) {
    guarded_void(*impl_, "detach", [&] {
        if (Instance* inst = impl_->find_instance(entity)) {
            impl_->destroy_instance(*inst, true);
        }
        impl_->compact();
    });
}

void ScriptVM::detach_all() {
    guarded_void(*impl_, "detach_all", [&] {
        for (usize i = impl_->instances.size(); i-- > 0;) {
            impl_->destroy_instance(*impl_->instances[i], true);
        }
        impl_->compact();
    });
}

ScriptInstanceState ScriptVM::instance_state(Entity entity) const { return impl_->state_of(entity); }

std::string ScriptVM::instance_error(Entity entity) const {
    const Instance* inst = impl_->find_instance(entity);
    return inst != nullptr && inst->failed ? inst->error : std::string();
}

usize ScriptVM::instance_count() const noexcept {
    usize n = 0;
    for (const auto& inst : impl_->instances) {
        n += inst->dead ? 0 : 1;
    }
    return n;
}

std::optional<ScriptValue> ScriptVM::get_field(Entity entity, std::string_view key) {
    return guarded(*impl_, "get_field", std::optional<ScriptValue>{},
                   [&]() -> std::optional<ScriptValue> {
                       const Instance* inst = impl_->find_instance(entity);
                       if (inst == nullptr) {
                           return std::nullopt;
                       }
                       return Impl::from_lua(inst->self.raw_get<sol::object>(key));
                   });
}

Result<void> ScriptVM::set_field(Entity entity, std::string_view key, const ScriptValue& value) {
    return guarded(*impl_, "set_field", vm_disabled(), [&]() -> Result<void> {
        Instance* inst = impl_->find_instance(entity);
        if (inst == nullptr) {
            return make_error(ErrorCode::NotFound, "no script instance for entity");
        }
        inst->self.raw_set(key, impl_->to_lua(value));
        return {};
    });
}

Result<std::vector<ScriptProperty>> ScriptVM::declared_properties(std::string_view script_path) {
    using R = Result<std::vector<ScriptProperty>>;
    return guarded(*impl_, "declared_properties",
                   R(Error{ ErrorCode::NotInitialized, "the scripting VM is disabled" }), [&]() -> R {
                       std::string path;
                       std::string err;
                       if (!normalize_script_path(script_path, path, err)) {
                           return R(Error{ ErrorCode::InvalidArgument, err });
                       }
                       ScriptModule& m = impl_->get_module(path);
                       impl_->flush();
                       if (!m.loaded) {
                           return R(Error{ ErrorCode::CompilationFailed, m.error });
                       }
                       std::vector<ScriptProperty> out;
                       if (m.properties.valid()) {
                           m.properties.for_each([&](const sol::object& k, const sol::object& v) {
                               if (k.get_type() != sol::type::string) {
                                   return;
                               }
                               if (auto value = Impl::from_lua(v)) {
                                   out.push_back(ScriptProperty{ k.as<std::string>(), std::move(*value) });
                               }
                           });
                       }
                       std::sort(out.begin(), out.end(), [](const ScriptProperty& a, const ScriptProperty& b) {
                           return a.name < b.name;
                       });
                       return R(std::move(out));
                   });
}

usize ScriptVM::reload_changed() {
    return guarded(*impl_, "reload_changed", usize{ 0 }, [&] {
        const usize n = impl_->reload_changed();
        impl_->flush();
        return n;
    });
}

Result<void> ScriptVM::reload(std::string_view script_path) {
    return guarded(*impl_, "reload", vm_disabled(), [&]() -> Result<void> {
        std::string path;
        std::string err;
        if (!normalize_script_path(script_path, path, err)) {
            return make_error(ErrorCode::InvalidArgument, err);
        }
        const auto it = impl_->modules.find(path);
        if (it == impl_->modules.end()) {
            return make_error(ErrorCode::NotFound, std::format("script '{}' is not loaded", path));
        }
        const bool ok = impl_->reload_module(*it->second);
        impl_->flush();
        if (!ok) {
            return make_error(ErrorCode::CompilationFailed, it->second->error.empty()
                                                                ? std::string("reload failed")
                                                                : it->second->error);
        }
        return {};
    });
}

Result<void> ScriptVM::run_string(std::string_view code, std::string_view chunk_name) {
    return guarded(*impl_, "run_string", vm_disabled(), [&]() -> Result<void> {
        lua_State*        L     = impl_->state();
        const std::string chunk = "=" + std::string(chunk_name);
        if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) {
            return make_error(ErrorCode::CompilationFailed, impl_->pop_error());
        }
        impl_->console_env.push(L);
        if (lua_setupvalue(L, -2, 1) == nullptr) {
            lua_pop(L, 1);
        }
        int status = LUA_OK;
        {
            OwnerScope scope(*impl_, 0);
            status = impl_->pcall(0, 0);
        }
        std::string err = status != LUA_OK ? impl_->pop_error() : std::string();
        impl_->flush();
        if (status != LUA_OK) {
            return make_error(ErrorCode::Unknown, std::move(err));
        }
        return {};
    });
}

Result<ScriptValue> ScriptVM::evaluate(std::string_view expression) {
    using R = Result<ScriptValue>;
    return guarded(*impl_, "evaluate", R(Error{ ErrorCode::NotInitialized, "the scripting VM is disabled" }),
                   [&]() -> R {
                       lua_State*        L    = impl_->state();
                       const std::string code = "return " + std::string(expression);
                       if (luaL_loadbufferx(L, code.data(), code.size(), "=eval", "t") != LUA_OK) {
                           return R(Error{ ErrorCode::CompilationFailed, impl_->pop_error() });
                       }
                       impl_->console_env.push(L);
                       if (lua_setupvalue(L, -2, 1) == nullptr) {
                           lua_pop(L, 1);
                       }
                       int status = LUA_OK;
                       {
                           OwnerScope scope(*impl_, 0);
                           status = impl_->pcall(0, 1);
                       }
                       if (status != LUA_OK) {
                           std::string err = impl_->pop_error();
                           impl_->flush();
                           return R(Error{ ErrorCode::Unknown, std::move(err) });
                       }
                       sol::object result(L, -1);
                       lua_pop(L, 1);
                       impl_->flush();
                       auto value = Impl::from_lua(result);
                       if (!value) {
                           return R(Error{ ErrorCode::Unsupported,
                                           "result is not representable as a ScriptValue" });
                       }
                       return R(std::move(*value));
                   });
}

std::optional<ScriptValue> ScriptVM::get_global(std::string_view name) {
    return guarded(*impl_, "get_global", std::optional<ScriptValue>{}, [&] {
        const sol::object o = impl_->console_env.get<sol::object>(name);
        return Impl::from_lua(o);
    });
}

usize ScriptVM::emit_event(std::string_view name, std::span<const ScriptProperty> payload) {
    return guarded(*impl_, "emit_event", usize{ 0 }, [&]() -> usize {
        lua_State* L = impl_->state();
        sol::table t = impl_->lua.create_table();
        for (const ScriptProperty& p : payload) {
            t.raw_set(p.name, impl_->to_lua(p.value));
        }
        impl_->emit_fn.push(L);
        lua_pushlstring(L, name.data(), name.size());
        t.push(L);
        int status = LUA_OK;
        {
            OwnerScope scope(*impl_, 0);
            status = impl_->pcall(2, 1);
        }
        usize delivered = 0;
        if (status == LUA_OK) {
            delivered = lua_isinteger(L, -1) != 0 ? static_cast<usize>(lua_tointeger(L, -1)) : 0;
            lua_pop(L, 1);
        } else {
            const std::string err = impl_->pop_error();
            ++impl_->error_count;
            AE_LOG_ERROR("Script", "emit_event('{}') failed.\n{}", name, err);
        }
        impl_->flush();
        return delivered;
    });
}

usize ScriptVM::memory_used() const noexcept { return impl_->alloc.used; }
u64   ScriptVM::error_count() const noexcept { return impl_->error_count; }
f64   ScriptVM::script_time() const noexcept { return impl_->time.time; }
u64   ScriptVM::frame_count() const noexcept { return impl_->time.frame; }

// =================================================================================================
// lua_integration.h
// =================================================================================================
sol::state& lua_state(ScriptVM& vm) { return vm.impl().lua; }

sol::table script_globals(ScriptVM& vm) { return vm.impl().base; }

void set_script_global(ScriptVM& vm, std::string_view name, const sol::object& value) {
    vm.impl().base.raw_set(name, value);
}

sol::table register_module(ScriptVM& vm, std::string_view name, const sol::table& module) {
    sol::table proxy = vm.impl().freeze(module);
    vm.impl().base.raw_set(name, proxy);
    return proxy;
}

sol::object to_lua(ScriptVM& vm, Entity entity) { return vm.impl().entity_object(entity); }

sol::object to_lua(ScriptVM& vm, const ScriptValue& value) { return vm.impl().to_lua(value); }

std::optional<ScriptValue> from_lua(const sol::object& value) { return ScriptVM::Impl::from_lua(value); }

} // namespace aether::scripting
