# Aether Lua scripting reference

Scripts are Lua 5.4 files under `content/scripts/`. Attach one to an entity with a
`ScriptComponent` (`script = "rotator.lua"`, in the editor inspector or in an `.aescene`
file). Each entity gets its own **instance**: a `self` table that holds that entity's
state, with the script's functions as methods.

```lua
-- rotator.lua
properties = { speed = 90, axis = Vec3(0, 1, 0) }   -- defaults; the inspector can override them

function on_start(self)
    log.info("rotating", self.entity:name())
end

function on_update(self, dt)
    self.entity:rotate(Quat.angle_axis(math.rad(self.speed) * dt, self.axis))
end
```

## Lifecycle

| Callback | When |
|---|---|
| `on_start(self)` | Once, before the instance's first update or fixed update. |
| `on_update(self, dt)` | Every frame (variable `dt`, in seconds). |
| `on_fixed_update(self, dt)` (alias `on_fixed`) | Every fixed simulation step (deterministic `dt`, 1/60 s by default). |
| `on_reload(self)` | After a hot reload of the script. `self` keeps its fields. |
| `on_destroy(self)` | When the component or entity goes away, or the VM shuts down. Only called if `on_start` ran. |

* `self.entity` is the owning entity.
* `properties = { ... }` declares defaults. They are copied onto `self` (tables are deep-copied
  per instance). Component overrides win over the defaults.
* A script may instead `return` a table of callbacks (module style); that table is used
  instead of the script's globals.
* Instances are created at the next update after the component appears, and all instances
  run in attach order.

## Errors, budgets and hot reload

* Any error in a callback disables **that instance only**. The error is logged once,
  with file, line and traceback, and other scripts keep running.
* Each call into Lua has an instruction budget (10M instructions by default) and the Lua
  heap has a memory limit (256 MB). A runaway loop is killed, even inside `pcall`.
* Saving a script reloads it. Instances keep `self` and get `on_reload(self)`. If the new
  file fails to compile, the previous version keeps running. Instances that had failed are
  revived and restart with `on_start`.

## Sandbox

Available: `assert error ipairs next pairs pcall xpcall rawequal rawget rawlen rawset select
tonumber tostring type setmetatable getmetatable print load` (text chunks only),
`collectgarbage` (`count/step/collect/isrunning`), and the `math string table coroutine utf8`
libraries. From `os`, only `os.clock` and `os.time` are available.

Not available: `io`, the rest of `os`, `package`, `debug`, `dofile`, `loadfile`,
`string.dump`.

* Every script and module has its own global environment, so globals never leak between
  scripts. Engine tables (`math`, `world`, `Vec3`, ...) are read-only.
* `require("lib.easing")` or `require("lib/easing.lua")` loads modules from the script root
  only. Modules are cached and cycles are reported as errors. Editing a module reloads every
  script that required it.

## Math

Vectors and quaternions are **mutable userdata**: `local b = a` makes `b` another name for
the same value. Use `Vec3(a)` to copy. Getters such as `e:position()` always return a
fresh copy.

| Type | Construct | Fields / methods |
|---|---|---|
| `Vec2` | `Vec2()`, `Vec2(s)`, `Vec2(x, y)`, `Vec2(v)` | `x y`, `length length2 normalized dot distance lerp`, `Vec2.zero() Vec2.one()` |
| `Vec3` | `Vec3()`, `Vec3(s)`, `Vec3(x, y, z)`, `Vec3(v)` | `x y z`, `length length2 normalized dot cross distance lerp`, `Vec3.zero() one() up() right() forward()` (forward is -Z) |
| `Vec4` | `Vec4(x, y, z, w)`, `Vec4(v3, w)` | `x y z w`, `xyz length normalized dot lerp` |
| `Quat` | `Quat()` (identity), `Quat(x, y, z, w)` | `x y z w`, `inverse normalized euler rotate dot`, `Quat.identity() from_euler(pitch, yaw, roll) angle_axis(rad, axis) look_rotation(fwd [, up]) slerp(a, b, t)` |
| `Transform` | `Transform([pos [, rot [, scale]]])` | `position rotation scale`, `transform_point transform_direction` |

Operators: `+ - * /` (component-wise, or with a scalar), unary `-`, `==`, `tostring`.
`Quat * Quat` composes rotations and `Quat * Vec3` rotates a vector. Angles are in radians.

## Entities

| Method | Description |
|---|---|
| `valid()` / `id()` / `uuid()` | Liveness, runtime id, persistent 16-hex-digit uuid. |
| `name()` / `set_name(s)`, `tag()` / `set_tag(n)` | Name and user tag. |
| `position()` `rotation()` `scale()` `transform()` | Local TRS (copies). |
| `set_position(v)` `set_rotation(q)` `set_scale(v or s)` `set_transform(t)` | Set local TRS. |
| `translate(v)` / `rotate(q)` | Add to the local position, or rotate in the local frame. |
| `world_position()` / `set_world_position(v)`, `world_rotation()` / `set_world_rotation(q)` | World space. |
| `forward()` `right()` `up()` | World-space axes. |
| `look_at(pos or entity)` | Point forward (-Z) at a target. |
| `parent()` / `set_parent(e or nil [, keep_world = true])` | Reparent. Returns `false` if the change would create a cycle. |
| `children()`, `child(name)`, `find("A/B")` | Hierarchy queries. |
| `visible()` / `set_visible(b)` | Own visibility flag (children inherit it). |
| `light()` / `add_light([kind])` / `remove_light()` | Light: `kind color intensity range inner_cone outer_cone cast_shadows`. |
| `camera()` / `add_camera()` / `remove_camera()` | Camera: `fov near far primary`. |
| `script()` / `has_script()` | The `self` table of another entity's script (to call its methods). |
| `add_script(path [, props])` / `remove_script()` | Deferred to the next update. |
| `destroy()` | Destroys the entity and its subtree after the current callbacks finish. |

Entities compare with `==` and can be used as table keys. Calling a method on a destroyed
entity raises an error. The only exceptions are `valid()` and `id()`.

## Globals

* **`world`**: `spawn([name [, parent]])`, `find(name)`, `find_path("A/B")`,
  `find_uuid(hex)`, `destroy(e)`, `count()`, `roots()`.
* **`input`**: `key_down/key_pressed/key_released(name)`,
  `mouse_down/mouse_pressed/mouse_released("left"|"right"|"middle"|"button4"|"button5")`,
  `mouse_position()`, `mouse_delta()`, `scroll()`, `axis(pos_key, neg_key)` (returns
  -1, 0 or 1), `available()`.
  * Key names: `a`–`z`, `0`–`9`, `f1`–`f12`, `space enter escape tab backspace insert delete
    left right up down page_up page_down home end caps_lock minus equal comma period slash
    semicolon apostrophe grave left_bracket right_bracket backslash`.
  * Modifiers: `shift ctrl alt super` match either side; `left_shift`, `right_ctrl` and so on
    match one side.
* **`time`** (read-only): `time` (script seconds), `dt` (the running callback's dt),
  `frame_dt`, `fixed_dt`, `fixed_time`, `frame`.
* **`timer`**: `after(seconds, fn)` and `every(seconds, fn)` return a handle with
  `cancel()` and `active()`. `timer.cancel(h)` also works. Callbacks receive the handle.
  Timers use script time and stop when their instance goes away.
* **`events`**: `subscribe(name, fn(payload, name))` returns a handle with `cancel()` and
  `active()`. Also `unsubscribe(h)`, `emit(name, payload)` (returns the number of handlers
  called) and `count(name)`. Handlers run synchronously. A failing handler is removed and
  disables its instance.
* **`log`**: `debug/info/warn/error(...)`. `print(...)` logs at info level. Every message
  carries its file and line.

Engine modules can add more tables, for example `physics`. See `lua_integration.h`.

## Visual scripts (node graphs)

A visual script is a `.aegraph` file: a Blueprint-style node graph that the engine compiles to Lua
when it loads it. Attach it exactly like a Lua script (`ScriptComponent` with
`script = "graphs/spin_and_hop.aegraph"`), so it gets the same per-entity instance, sandbox,
error isolation and hot reload.

* **Create one:** select an entity, then in the inspector's *Script* section press
  **New Visual Script** (with an empty script path; add a *Script* component first). This makes
  `scripts/<entity name>.aegraph` with a starter graph and opens the **Visual Script** window.
  Double-click an `.aegraph` file in the Assets panel to edit it, or press **Edit Graph** in the
  inspector.
* **Events** (red) start execution: *On Start*, *On Update*, *On Key Pressed / Released*,
  *On Event* (fired by *Emit Event* from any script) and *Every N Seconds*.
* **Execution wires** (white) run nodes in order. *Branch*, *Sequence* and *Delay* control the
  flow. Actions: *Print*, *Set Position*, *Set World Position*, *Move By*, *Rotate*,
  *Set Scale*, *Set Visible*, *Look At*, *Spawn Entity*, *Destroy Entity*, *Emit Event* and
  *Set Variable*.
* **Data wires** (coloured by type: green number, yellow vector, red bool, pink text, blue
  entity) feed values. They come from math, logic, vector, input, time and entity nodes. An
  unconnected input uses the value typed into the node. Entity inputs default to **Self**.
* **Variables** (left panel) are the graph's per-entity values. They become the script's
  `properties`, so the inspector can override them per entity.
* **Editing:**
  * Right-click the canvas, or drop a wire on empty space, to add a node. The list is filtered
    to nodes that fit the wire.
  * Drag between pins to connect them. Dragging a connected input picks up its wire, and
    Alt+click clears a pin.
  * Right-drag pans the canvas and the mouse wheel zooms.
  * Del deletes, Ctrl+D duplicates, Ctrl+Z / Ctrl+Y undo and redo, and Ctrl+S saves.
* **Errors:** nodes with errors are outlined red, with a clickable list below the canvas. A
  graph with errors does not replace the running version.
* **Generated Lua:** *View > Show generated Lua* shows the code the graph turns into, which is
  handy for learning the Lua API.

