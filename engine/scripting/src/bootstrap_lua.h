// bootstrap_lua.h — PRIVATE: Lua source run once per VM, UNSANDBOXED, before any user script.
//
// Receives the C++ `host` table and returns (base, internal):
//   base     — the sandbox global table every script environment falls back to;
//   internal — env_mt, freeze, make_instance_mt, emit, purge_owner, loaded (require cache).
// Everything that must not be observable/modifiable by scripts is captured in upvalues here.
// (Kept below MSVC's 16 KB-per-literal limit by splitting into two raw strings.)
#pragma once

namespace aether::scripting::detail {

inline constexpr const char kBootstrapPart1[] = R"lua(
local host = ...
local SENTINEL = host.budget_sentinel

local raw_pcall, raw_xpcall, raw_error = pcall, xpcall, error
local raw_type, raw_select, raw_tostring = type, select, tostring
local raw_rawequal, raw_rawget, raw_rawset = rawequal, rawget, rawset
local raw_setmetatable, raw_getmetatable = setmetatable, getmetatable
local raw_next, raw_load, raw_collectgarbage = next, load, collectgarbage
local t_concat, t_remove, t_move = table.concat, table.remove, table.move
local co_resume, co_close = coroutine.resume, coroutine.close
local host_log, host_traceback = host.log, host.traceback
local host_set_owner, host_current_owner = host.set_owner, host.current_owner
local host_handler_failed = host.handler_failed

-- read-only proxies -------------------------------------------------------------------------
local FROZEN = raw_setmetatable({}, { __mode = "k" })
local function readonly_newindex(_, key)
  raw_error("attempt to modify read-only table (field '" .. raw_tostring(key) .. "')", 2)
end
local function freeze(t)
  local proxy = raw_setmetatable({}, {
    __index = t,
    __newindex = readonly_newindex,
    __pairs = function() return raw_next, t, nil end,
    __len = function() return #t end,
    __metatable = false,
  })
  FROZEN[proxy] = true
  return proxy
end
local function copy(t, exclude)
  local c = {}
  for k, v in raw_next, t do
    if not (exclude and exclude[k]) then c[k] = v end
  end
  return c
end

-- protected calls that never swallow the instruction-budget kill -------------------------------
local function propagate(ok, ...)
  if not ok and raw_rawequal((...), SENTINEL) then raw_error(SENTINEL, 0) end
  return ok, ...
end
local function safe_pcall(f, ...)
  return propagate(raw_pcall(f, ...))
end
local function safe_xpcall(f, handler, ...)
  if raw_type(handler) ~= "function" then
    raw_error("bad argument #2 to 'xpcall' (function expected, got " .. raw_type(handler) .. ")", 2)
  end
  return propagate(raw_xpcall(f, function(e)
    if raw_rawequal(e, SENTINEL) then return e end
    return handler(e)
  end, ...))
end
local function safe_rawset(t, k, v)
  if FROZEN[t] then raw_error("attempt to modify read-only table", 2) end
  return raw_rawset(t, k, v)
end

local new_env -- defined once `base` exists

local function safe_load(chunk, chunkname, mode, env)
  if mode ~= nil and mode ~= "t" then
    return nil, "only text chunks can be loaded (mode must be 't')"
  end
  if env == nil then env = new_env() end
  return raw_load(chunk, chunkname, "t", env)
end

local GC_ALLOWED = { count = true, step = true, collect = true, isrunning = true }
local function safe_collectgarbage(opt, ...)
  if opt == nil then opt = "collect" end
  if not GC_ALLOWED[opt] then
    raw_error("collectgarbage('" .. raw_tostring(opt) .. "') is not available to scripts", 2)
  end
  return raw_collectgarbage(opt, ...)
end

-- logging (host.log reports the location of the Lua function two levels up) ------------------
local function stringify(sep, ...)
  local n = raw_select("#", ...)
  local parts = {}
  for i = 1, n do parts[i] = raw_tostring((raw_select(i, ...))) end
  return t_concat(parts, sep)
end
local function safe_print(...) host_log(1, stringify("\t", ...)) end
local log = {
  debug = function(...) host_log(0, stringify(" ", ...)) end,
  info  = function(...) host_log(1, stringify(" ", ...)) end,
  warn  = function(...) host_log(2, stringify(" ", ...)) end,
  error = function(...) host_log(3, stringify(" ", ...)) end,
}

-- require: modules under the script root only, cached, cycle-safe -----------------------------
local loaded, loading = {}, {}
local host_resolve, host_compile = host.resolve_module, host.compile_module
local host_note_dependency, host_end_module = host.note_dependency, host.end_module
local function safe_require(name)
  if raw_type(name) ~= "string" then
    raw_error("bad argument #1 to 'require' (string expected, got " .. raw_type(name) .. ")", 2)
  end
  local key, err = host_resolve(name)
  if key == nil then raw_error(err, 2) end
  host_note_dependency(key)
  local value = loaded[key]
  if value ~= nil then return value end
  if loading[key] then raw_error("circular require of '" .. name .. "' (" .. key .. ")", 2) end
  local chunk, owner = host_compile(key)
  if chunk == nil then raw_error(owner, 2) end
  loading[key] = true
  local previous_owner = host_set_owner(owner)
  local guard <close> = raw_setmetatable({}, { __close = function()
    loading[key] = nil
    host_set_owner(previous_owner)
    host_end_module(key)
  end })
  local result = chunk(name, key)
  if result == nil then result = true end
  loaded[key] = result
  return result
end
)lua";

inline constexpr const char kBootstrapPart2[] = R"lua(
-- events: synchronous, snapshot-iterated, failures isolated per handler ----------------------
local subscriptions = {}
local HANDLES = raw_setmetatable({}, { __mode = "k" })

local function remove_subscription(rec)
  if not rec.alive then return end
  rec.alive = false
  local list = subscriptions[rec.name]
  if list == nil then return end
  for i = #list, 1, -1 do
    if list[i] == rec then t_remove(list, i) break end
  end
  if #list == 0 then subscriptions[rec.name] = nil end
end

local SubscriptionMT = { __metatable = false }
SubscriptionMT.__index = {
  cancel = function(h) local rec = HANDLES[h]; if rec then remove_subscription(rec) end end,
  active = function(h) local rec = HANDLES[h]; return rec ~= nil and rec.alive end,
}
SubscriptionMT.__tostring = function(h)
  local rec = HANDLES[h]
  return "Subscription(" .. (rec and rec.name or "?") .. ")"
end

local function check_event_name(name, fname)
  if raw_type(name) ~= "string" then
    raw_error("bad argument #1 to '" .. fname .. "' (string expected, got " .. raw_type(name) .. ")", 3)
  end
end

local events = {}
function events.subscribe(name, fn)
  check_event_name(name, "events.subscribe")
  if raw_type(fn) ~= "function" then
    raw_error("bad argument #2 to 'events.subscribe' (function expected, got " .. raw_type(fn) .. ")", 2)
  end
  local rec = { name = name, fn = fn, owner = host_current_owner(), alive = true }
  local list = subscriptions[name]
  if list == nil then list = {}; subscriptions[name] = list end
  list[#list + 1] = rec
  local handle = raw_setmetatable({}, SubscriptionMT)
  HANDLES[handle] = rec
  return handle
end
function events.unsubscribe(handle)
  local rec = HANDLES[handle]
  if rec == nil then return false end
  local was_alive = rec.alive
  remove_subscription(rec)
  return was_alive
end
function events.count(name)
  check_event_name(name, "events.count")
  local list = subscriptions[name]
  return list and #list or 0
end

local function handler_message(e)
  if raw_rawequal(e, SENTINEL) then return e end
  return host_traceback(raw_tostring(e), 2)
end

local function emit(name, payload)
  check_event_name(name, "events.emit")
  local list = subscriptions[name]
  if list == nil then return 0 end
  local n = #list
  local snapshot = t_move(list, 1, n, 1, {})
  local delivered = 0
  for i = 1, n do
    local rec = snapshot[i]
    if rec.alive then
      local previous = host_set_owner(rec.owner)
      local ok, err = raw_xpcall(rec.fn, handler_message, payload, name)
      host_set_owner(previous)
      if ok then
        delivered = delivered + 1
      elseif raw_rawequal(err, SENTINEL) then
        raw_error(SENTINEL, 0)
      else
        remove_subscription(rec)
        host_handler_failed(rec.owner, "event '" .. name .. "' handler", err)
      end
    end
  end
  return delivered
end
events.emit = emit

local function purge_owner(owner)
  for name, list in raw_next, subscriptions do
    for i = #list, 1, -1 do
      local rec = list[i]
      if rec.owner == owner then
        rec.alive = false
        t_remove(list, i)
      end
    end
    if #list == 0 then subscriptions[name] = nil end
  end
end

-- per-module instance metatable: methods resolve to the module's own fields only --------------
local function make_instance_mt(module)
  local mt = { module = module, __metatable = false }
  mt.__index = function(_, key) return raw_rawget(mt.module, key) end
  return mt
end

-- the sandbox base ---------------------------------------------------------------------------
local base = {}
for _, name in ipairs({ "assert", "error", "ipairs", "next", "pairs", "rawequal", "rawget",
                        "rawlen", "select", "tonumber", "tostring", "type",
                        "setmetatable", "getmetatable" }) do
  base[name] = _G[name]
end
base._VERSION = _VERSION
base.rawset = safe_rawset
base.pcall = safe_pcall
base.xpcall = safe_xpcall
base.print = safe_print
base.load = safe_load
base.collectgarbage = safe_collectgarbage
base.require = safe_require

local sandbox_coroutine = copy(coroutine)
sandbox_coroutine.resume = function(co, ...) return propagate(co_resume(co, ...)) end
sandbox_coroutine.close = function(co) return propagate(co_close(co)) end

base.math = freeze(copy(math))
base.string = freeze(copy(string, { dump = true }))
base.table = freeze(copy(table))
base.coroutine = freeze(sandbox_coroutine)
base.utf8 = freeze(copy(utf8))
base.os = freeze({ clock = os.clock, time = os.time })
base.log = freeze(log)
base.events = freeze(events)

local ENV_MT = { __index = base, __metatable = false }
new_env = function()
  local env = raw_setmetatable({}, ENV_MT)
  env._G = env
  return env
end

-- harden the host state itself (scripts never see these globals; defense in depth)
string.dump = nil
raw_getmetatable("").__metatable = false
dofile, loadfile = nil, nil

return base, {
  env_mt = ENV_MT,
  freeze = freeze,
  make_instance_mt = make_instance_mt,
  emit = emit,
  purge_owner = purge_owner,
  loaded = loaded,
}
)lua";

} // namespace aether::scripting::detail
