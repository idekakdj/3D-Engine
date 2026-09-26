// lua_bindings.h — PRIVATE: script APIs that gameplay adds on top of the scripting module
// (ADR-0003: Layer-4a modules never bind each other; the assembly layer does).
//
//   physics.raycast(origin, dir, max_distance [, ignore]) -> { entity, point, normal, distance } | nil
//   physics.overlap_sphere(center, radius) -> { entity, ... }
//   physics.velocity(e) / physics.set_velocity(e, v) / physics.add_impulse(e, v) /
//   physics.add_force(e, v) / physics.gravity()
//   physics.move_character(e, horizontal_velocity) / physics.jump(e) / physics.on_ground(e)
//   actions.down(name) / actions.pressed(name) / actions.released(name) / actions.axis(name)
// dispatch_contact_events() forwards the frame's physics contacts as script events
// "physics.contact_begin" / "physics.contact_end" with payload
// { a, b, point, normal, speed, trigger }.
//
// Only compiled with AE_WITH_SCRIPTING. Main thread only.
#pragma once

#include "aether/core/types.h"

namespace aether::physics {
class PhysicsSubsystem;
}
namespace aether::scripting {
class ScriptVM;
}

namespace aether::gameplay {

class InputSubsystem;

struct LuaBindingContext {
    physics::PhysicsSubsystem* physics = nullptr;
    InputSubsystem*            input   = nullptr;
};

void register_gameplay_lua_bindings(scripting::ScriptVM& vm, const LuaBindingContext& ctx);

// Returns the number of events emitted.
usize dispatch_contact_events(scripting::ScriptVM& vm, physics::PhysicsSubsystem& physics);

} // namespace aether::gameplay
