// lua_bindings.cpp — gameplay's script APIs (see lua_bindings.h). The only sol2 TU of gameplay.
#include "lua_bindings.h"

#if AE_WITH_SCRIPTING

#    include "aether/gameplay/input_subsystem.h"
#    include "aether/physics/components.h"
#    include "aether/physics/physics_subsystem.h"
#    include "aether/scene/world.h"
#    include "aether/scripting/lua_integration.h"

#    include <cmath>

namespace aether::gameplay {

namespace {

using scripting::raise_script_error;

physics::PhysicsWorld& world_or_raise(lua_State* L, physics::PhysicsSubsystem* sys) {
    physics::PhysicsWorld* pw = sys != nullptr ? sys->physics_world() : nullptr;
    if (pw == nullptr) {
        raise_script_error(L, "physics is not running");
    }
    return *pw;
}

Entity entity_or_raise(lua_State* L, const sol::object& o, const char* fn) {
    const auto e = scripting::entity_from_lua(o);
    if (!e || *e == kNullEntity) {
        raise_script_error(L, std::string(fn) + ": expected an Entity");
    }
    return *e;
}

Vec3 finite_vec(lua_State* L, const Vec3& v, const char* fn) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        raise_script_error(L, std::string(fn) + ": vector must be finite");
    }
    return v;
}

sol::table hit_table(scripting::ScriptVM& vm, lua_State* L, const physics::QueryHit& h) {
    sol::table t = sol::state_view(L).create_table();
    t["entity"]   = scripting::to_lua(vm, h.entity);
    t["point"]    = h.point;
    t["normal"]   = h.normal;
    t["distance"] = h.distance;
    return t;
}

} // namespace

void register_gameplay_lua_bindings(scripting::ScriptVM& vm, const LuaBindingContext& ctx) {
    sol::state&                lua     = scripting::lua_state(vm);
    physics::PhysicsSubsystem* phys    = ctx.physics;
    InputSubsystem*            input   = ctx.input;
    scripting::ScriptVM*       vm_ptr  = &vm;

    if (phys != nullptr) {
        sol::table p = lua.create_table();
        p.set_function("raycast", [phys, vm_ptr](sol::this_state s, const Vec3& origin, const Vec3& dir,
                                                 f32 max_distance, const sol::object& ignore) -> sol::object {
            physics::PhysicsWorld& pw = world_or_raise(s, phys);
            if (glm::dot(dir, dir) <= 0.0f || !(max_distance > 0.0f)) {
                raise_script_error(s, "physics.raycast: direction must be non-zero and max_distance > 0");
            }
            physics::QueryFilter filter;
            if (const auto e = scripting::entity_from_lua(ignore)) {
                filter.ignore_entity = *e;
            }
            const auto hit = pw.raycast(finite_vec(s, origin, "physics.raycast"), finite_vec(s, dir, "physics.raycast"),
                                        max_distance, filter);
            if (!hit) {
                return sol::make_object(s, sol::lua_nil);
            }
            return hit_table(*vm_ptr, s, *hit);
        });
        p.set_function("overlap_sphere", [phys, vm_ptr](sol::this_state s, const Vec3& center, f32 radius) {
            physics::PhysicsWorld& pw = world_or_raise(s, phys);
            if (!(radius > 0.0f)) {
                raise_script_error(s, "physics.overlap_sphere: radius must be > 0");
            }
            sol::table out = sol::state_view(s).create_table();
            int        i   = 1;
            for (const physics::QueryHit& h : pw.overlap_sphere(finite_vec(s, center, "physics.overlap_sphere"), radius)) {
                out.raw_set(i++, scripting::to_lua(*vm_ptr, h.entity));
            }
            return out;
        });
        p.set_function("velocity", [phys](sol::this_state s, const sol::object& e) {
            return world_or_raise(s, phys).linear_velocity(entity_or_raise(s, e, "physics.velocity"));
        });
        p.set_function("set_velocity", [phys](sol::this_state s, const sol::object& e, const Vec3& v) {
            world_or_raise(s, phys).set_linear_velocity(entity_or_raise(s, e, "physics.set_velocity"),
                                                        finite_vec(s, v, "physics.set_velocity"));
        });
        p.set_function("add_impulse", [phys](sol::this_state s, const sol::object& e, const Vec3& v) {
            world_or_raise(s, phys).add_impulse(entity_or_raise(s, e, "physics.add_impulse"),
                                                finite_vec(s, v, "physics.add_impulse"));
        });
        p.set_function("add_force", [phys](sol::this_state s, const sol::object& e, const Vec3& v) {
            world_or_raise(s, phys).add_force(entity_or_raise(s, e, "physics.add_force"),
                                              finite_vec(s, v, "physics.add_force"));
        });
        p.set_function("gravity", [phys](sol::this_state s) { return world_or_raise(s, phys).gravity(); });
        p.set_function("move_character", [vm_ptr](sol::this_state s, const sol::object& e, const Vec3& v) {
            const Entity id = entity_or_raise(s, e, "physics.move_character");
            auto*        cc = vm_ptr->world().try_get<physics::CharacterControllerComponent>(id);
            if (cc == nullptr) {
                raise_script_error(s, "physics.move_character: entity has no CharacterControllerComponent");
            }
            cc->desired_velocity = finite_vec(s, v, "physics.move_character");
        });
        p.set_function("jump", [vm_ptr](sol::this_state s, const sol::object& e) {
            const Entity id = entity_or_raise(s, e, "physics.jump");
            auto*        cc = vm_ptr->world().try_get<physics::CharacterControllerComponent>(id);
            if (cc == nullptr) {
                raise_script_error(s, "physics.jump: entity has no CharacterControllerComponent");
            }
            cc->jump_requested = true;
        });
        p.set_function("on_ground", [vm_ptr](sol::this_state s, const sol::object& e) {
            const Entity id = entity_or_raise(s, e, "physics.on_ground");
            const auto*  cc = vm_ptr->world().try_get<physics::CharacterControllerComponent>(id);
            return cc != nullptr && cc->on_ground;
        });
        scripting::register_module(vm, "physics", p);
    }

    if (input != nullptr) {
        sol::table a = lua.create_table();
        a.set_function("down", [input](std::string_view name) { return input->input_map().action_down(name); });
        a.set_function("pressed", [input](std::string_view name) { return input->input_map().action_pressed(name); });
        a.set_function("released",
                       [input](std::string_view name) { return input->input_map().action_released(name); });
        a.set_function("axis", [input](std::string_view name) { return input->input_map().axis(name); });
        scripting::register_module(vm, "actions", a);
    }
}

usize dispatch_contact_events(scripting::ScriptVM& vm, physics::PhysicsSubsystem& physics_sys) {
    physics::PhysicsWorld* pw = physics_sys.physics_world();
    if (pw == nullptr) {
        return 0;
    }
    usize emitted = 0;
    for (const physics::ContactEvent& ev : pw->contact_events()) {
        if (ev.type == physics::ContactEventType::Persist) {
            continue;
        }
        const scripting::ScriptProperty payload[] = {
            { "a", ev.a },
            { "b", ev.b },
            { "point", ev.point },
            { "normal", ev.normal },
            { "speed", static_cast<f64>(ev.approach_speed) },
            { "trigger", ev.is_trigger },
        };
        vm.emit_event(ev.type == physics::ContactEventType::Begin ? "physics.contact_begin" : "physics.contact_end",
                      payload);
        ++emitted;
    }
    return emitted;
}

} // namespace aether::gameplay

#else

namespace aether::gameplay {
void  register_gameplay_lua_bindings(scripting::ScriptVM&, const LuaBindingContext&) {}
usize dispatch_contact_events(scripting::ScriptVM&, physics::PhysicsSubsystem&) { return 0; }
} // namespace aether::gameplay

#endif
