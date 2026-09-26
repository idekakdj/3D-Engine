// aether/scripting/lua_integration.h — OPT-IN escape hatch to the ScriptVM's sol2/Lua state.
//
// *** NOT part of the regular scripting API. ***
// Including this header pulls in sol2 + Lua (very compile-heavy) and requires linking
// `aether::scripting_lua`, which provides sol2 with EXACTLY the module's configuration
// (SOL_ALL_SAFETIES_ON=1; mixing sol2 configurations across TUs is an ODR violation).
// Keep it confined to one TU per consumer. It exists for Layer-5 glue (gameplay/editor) that
// registers extra script APIs — e.g. `physics.raycast` — because Layer-4 modules never
// register bindings for each other (ADR-0001/ADR-0003).
//
// Rules for binding authors
//   * Scripts run in sandboxed environments: values written to lua_state().globals() are NOT
//     visible to scripts. Publish with register_module() / set_script_global().
//   * Report misuse from a bound C++ function with raise_script_error(): sol2 converts it into
//     a Lua error attributed to the calling script (file:line), which disables only that script
//     instance. Never call lua_error/luaL_error from C++ frames that own objects with destructors.
//   * Convert entities with to_lua()/entity_from_lua() so scripts receive the canonical Entity
//     userdata (identity-stable, usable as table keys, full entity API).
//   * Everything here is main-thread only; do not keep sol2 references alive past the VM.
//
// Typical use (from ScriptingSubsystem::add_binding_registrar):
//     subsystem.add_binding_registrar([&](scripting::ScriptVM& vm) {
//         sol::state& lua = scripting::lua_state(vm);
//         sol::table physics = lua.create_table();
//         physics.set_function("raycast", [&vm](const Vec3& from, const Vec3& dir, f32 dist) {...});
//         scripting::register_module(vm, "physics", physics);   // scripts: physics.raycast(...)
//     });
#pragma once

#include "aether/scripting/script_vm.h"

#include <sol/sol.hpp>

#include <optional>
#include <string_view>

namespace aether::scripting {

// The VM's sol2 state (main thread only).
[[nodiscard]] sol::state& lua_state(ScriptVM& vm);

// The sandbox base table every script environment falls back to (read-mostly; prefer
// register_module / set_script_global to add entries).
[[nodiscard]] sol::table script_globals(ScriptVM& vm);

// Publishes `value` to every script (existing and future) under the global `name`.
void set_script_global(ScriptVM& vm, std::string_view name, const sol::object& value);

// Publishes `module` as a READ-ONLY global table `name` (scripts cannot modify or replace its
// fields; pairs()/# still work). Returns the read-only proxy that scripts see.
sol::table register_module(ScriptVM& vm, std::string_view name, const sol::table& module);

// Canonical Entity userdata (nil for kNullEntity) / entity from a script value.
[[nodiscard]] sol::object           to_lua(ScriptVM& vm, Entity entity);
[[nodiscard]] sol::object           to_lua(ScriptVM& vm, const ScriptValue& value);
[[nodiscard]] std::optional<Entity> entity_from_lua(const sol::object& value);
[[nodiscard]] std::optional<ScriptValue> from_lua(const sol::object& value);

// Raises a Lua error "<script>:<line>: <message>" from inside a sol2-bound function.
[[noreturn]] void raise_script_error(lua_State* L, std::string_view message);

} // namespace aether::scripting
