// bindings.cpp — the engine API scripts see: math types, Transform, Entity (+ Light/Camera
// component references), the `world`, `input`, `time` and `timer` globals. One of the two sol2
// TUs (see vm_impl.h). The script-facing reference is content/scripts/README.md.
//
// Conventions
//   * Everything is published through impl.base (the sandbox globals); constructor tables are
//     read-only proxies (Vec3.x = 1 raises), callable as Vec3(1, 2, 3).
//   * Misuse raises a Lua error attributed to the calling script via raise_script_error();
//     the failing instance is disabled by the VM, the engine never crashes.
//   * Entities are canonical userdata (ScriptVM::Impl::entity_object): identity-stable, so they
//     work as table keys and compare with ==. Methods on a destroyed entity raise, except
//     valid()/id().
//   * Structural changes (destroy, add/remove script) are deferred to the VM's next flush, so
//     running iterations never see entities vanish under them.
#include "vm_impl.h"

#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace aether::scripting {

namespace {

using Impl = ScriptVM::Impl;

// ---- small helpers ---------------------------------------------------------------------------
Impl& impl_of(lua_State* L) { return Impl::from(L); }

[[nodiscard]] f32 finite_or_raise(lua_State* L, f64 v, std::string_view what) {
    if (!std::isfinite(v)) {
        raise_script_error(L, std::format("{} must be a finite number", what));
    }
    return static_cast<f32>(v);
}

// Valid entity id of `e`, or a script error.
Entity checked(lua_State* L, const LuaEntity& e) {
    if (e.id == kNullEntity || !impl_of(L).world.valid(e.id)) {
        raise_script_error(L, "entity is no longer valid");
    }
    return e.id;
}

sol::object entity_or_nil(lua_State* L, Entity e) {
    return impl_of(L).entity_object(e, L);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

template <typename T>
std::string fmt_float(T v) {
    return std::format("{:.4g}", static_cast<f64>(v));
}

// Makes a read-only proxy of `t` callable: Proxy(...) == t.new(...).
sol::table callable_proxy(Impl& impl, const sol::table& t) {
    sol::table proxy = impl.freeze(t);
    lua_State* L     = impl.state();
    static constexpr std::string_view kForward =
        "local ctor = ...; return function(_, ...) return ctor(...) end";
    if (luaL_loadbufferx(L, kForward.data(), kForward.size(), "=[aether.bindings]", "t") != LUA_OK) {
        impl.internal_error("bindings", impl.pop_error());
        return proxy;
    }
    t.get<sol::object>("new").push(L);
    if (impl.pcall(1, 1) != LUA_OK) {
        impl.internal_error("bindings", impl.pop_error());
        return proxy;
    }
    proxy.push(L);                       // [call, proxy]
    if (lua_getmetatable(L, -1) != 0) {  // [call, proxy, mt]
        lua_pushvalue(L, -3);            // [call, proxy, mt, call]
        lua_setfield(L, -2, "__call");   // [call, proxy, mt]
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    return proxy;
}

// Hides a usertype's metatable from getmetatable()/setmetatable() in scripts.
void harden(lua_State* L, const sol::object& sample) {
    sample.push(L);
    if (lua_getmetatable(L, -1) != 0) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// Publishes the usertype table registered as host global `name` as a callable read-only proxy.
void publish_type(Impl& impl, const char* name) {
    sol::table t = impl.lua.get<sol::table>(name);
    impl.base.raw_set(name, callable_proxy(impl, t));
    impl.lua.set(name, sol::lua_nil); // host globals are invisible to scripts anyway; keep tidy
}

// =================================================================================================
// math
// =================================================================================================
template <typename V>
V safe_normalize(const V& v) {
    const f32 len = glm::length(v);
    return len > 1e-12f ? v / len : V(0.0f);
}

void register_math(Impl& impl) {
    sol::state& lua = impl.lua;

    // ---- Vec2 --------------------------------------------------------------------------------
    lua.new_usertype<Vec2>(
        "Vec2", sol::no_constructor,
        "new", sol::overload([]() { return Vec2(0.0f); }, [](f32 s) { return Vec2(s); },
                             [](f32 x, f32 y) { return Vec2(x, y); },
                             [](const Vec2& v) { return v; }),
        "x", sol::property([](const Vec2& v) { return v.x; }, [](Vec2& v, f32 s) { v.x = s; }),
        "y", sol::property([](const Vec2& v) { return v.y; }, [](Vec2& v, f32 s) { v.y = s; }),
        "length", [](const Vec2& v) { return glm::length(v); },
        "length2", [](const Vec2& v) { return glm::dot(v, v); },
        "normalized", [](const Vec2& v) { return safe_normalize(v); },
        "dot", [](const Vec2& a, const Vec2& b) { return glm::dot(a, b); },
        "distance", [](const Vec2& a, const Vec2& b) { return glm::distance(a, b); },
        "lerp", [](const Vec2& a, const Vec2& b, f32 t) { return glm::mix(a, b, t); },
        "zero", []() { return Vec2(0.0f); },
        "one", []() { return Vec2(1.0f); },
        sol::meta_function::addition, [](const Vec2& a, const Vec2& b) { return a + b; },
        sol::meta_function::subtraction, [](const Vec2& a, const Vec2& b) { return a - b; },
        sol::meta_function::multiplication,
        sol::overload([](const Vec2& a, const Vec2& b) { return a * b; },
                      [](const Vec2& a, f32 s) { return a * s; },
                      [](f32 s, const Vec2& a) { return s * a; }),
        sol::meta_function::division,
        sol::overload([](const Vec2& a, const Vec2& b) { return a / b; },
                      [](const Vec2& a, f32 s) { return a / s; }),
        sol::meta_function::unary_minus, [](const Vec2& a) { return -a; },
        sol::meta_function::equal_to, [](const Vec2& a, const Vec2& b) { return a == b; },
        sol::meta_function::to_string,
        [](const Vec2& v) { return std::format("Vec2({}, {})", fmt_float(v.x), fmt_float(v.y)); });

    // ---- Vec3 --------------------------------------------------------------------------------
    lua.new_usertype<Vec3>(
        "Vec3", sol::no_constructor,
        "new", sol::overload([]() { return Vec3(0.0f); }, [](f32 s) { return Vec3(s); },
                             [](f32 x, f32 y, f32 z) { return Vec3(x, y, z); },
                             [](const Vec3& v) { return v; }),
        "x", sol::property([](const Vec3& v) { return v.x; }, [](Vec3& v, f32 s) { v.x = s; }),
        "y", sol::property([](const Vec3& v) { return v.y; }, [](Vec3& v, f32 s) { v.y = s; }),
        "z", sol::property([](const Vec3& v) { return v.z; }, [](Vec3& v, f32 s) { v.z = s; }),
        "length", [](const Vec3& v) { return glm::length(v); },
        "length2", [](const Vec3& v) { return glm::dot(v, v); },
        "normalized", [](const Vec3& v) { return safe_normalize(v); },
        "dot", [](const Vec3& a, const Vec3& b) { return glm::dot(a, b); },
        "cross", [](const Vec3& a, const Vec3& b) { return glm::cross(a, b); },
        "distance", [](const Vec3& a, const Vec3& b) { return glm::distance(a, b); },
        "lerp", [](const Vec3& a, const Vec3& b, f32 t) { return glm::mix(a, b, t); },
        "zero", []() { return Vec3(0.0f); },
        "one", []() { return Vec3(1.0f); },
        "up", []() { return Vec3(0.0f, 1.0f, 0.0f); },
        "right", []() { return Vec3(1.0f, 0.0f, 0.0f); },
        "forward", []() { return Vec3(0.0f, 0.0f, -1.0f); }, // right-handed: -Z is forward
        sol::meta_function::addition, [](const Vec3& a, const Vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const Vec3& a, const Vec3& b) { return a - b; },
        sol::meta_function::multiplication,
        sol::overload([](const Vec3& a, const Vec3& b) { return a * b; },
                      [](const Vec3& a, f32 s) { return a * s; },
                      [](f32 s, const Vec3& a) { return s * a; }),
        sol::meta_function::division,
        sol::overload([](const Vec3& a, const Vec3& b) { return a / b; },
                      [](const Vec3& a, f32 s) { return a / s; }),
        sol::meta_function::unary_minus, [](const Vec3& a) { return -a; },
        sol::meta_function::equal_to, [](const Vec3& a, const Vec3& b) { return a == b; },
        sol::meta_function::to_string, [](const Vec3& v) {
            return std::format("Vec3({}, {}, {})", fmt_float(v.x), fmt_float(v.y), fmt_float(v.z));
        });

    // ---- Vec4 --------------------------------------------------------------------------------
    lua.new_usertype<Vec4>(
        "Vec4", sol::no_constructor,
        "new", sol::overload([]() { return Vec4(0.0f); }, [](f32 s) { return Vec4(s); },
                             [](f32 x, f32 y, f32 z, f32 w) { return Vec4(x, y, z, w); },
                             [](const Vec3& v, f32 w) { return Vec4(v, w); },
                             [](const Vec4& v) { return v; }),
        "x", sol::property([](const Vec4& v) { return v.x; }, [](Vec4& v, f32 s) { v.x = s; }),
        "y", sol::property([](const Vec4& v) { return v.y; }, [](Vec4& v, f32 s) { v.y = s; }),
        "z", sol::property([](const Vec4& v) { return v.z; }, [](Vec4& v, f32 s) { v.z = s; }),
        "w", sol::property([](const Vec4& v) { return v.w; }, [](Vec4& v, f32 s) { v.w = s; }),
        "xyz", [](const Vec4& v) { return Vec3(v); },
        "length", [](const Vec4& v) { return glm::length(v); },
        "normalized", [](const Vec4& v) { return safe_normalize(v); },
        "dot", [](const Vec4& a, const Vec4& b) { return glm::dot(a, b); },
        "lerp", [](const Vec4& a, const Vec4& b, f32 t) { return glm::mix(a, b, t); },
        sol::meta_function::addition, [](const Vec4& a, const Vec4& b) { return a + b; },
        sol::meta_function::subtraction, [](const Vec4& a, const Vec4& b) { return a - b; },
        sol::meta_function::multiplication,
        sol::overload([](const Vec4& a, const Vec4& b) { return a * b; },
                      [](const Vec4& a, f32 s) { return a * s; },
                      [](f32 s, const Vec4& a) { return s * a; }),
        sol::meta_function::division,
        sol::overload([](const Vec4& a, const Vec4& b) { return a / b; },
                      [](const Vec4& a, f32 s) { return a / s; }),
        sol::meta_function::unary_minus, [](const Vec4& a) { return -a; },
        sol::meta_function::equal_to, [](const Vec4& a, const Vec4& b) { return a == b; },
        sol::meta_function::to_string, [](const Vec4& v) {
            return std::format("Vec4({}, {}, {}, {})", fmt_float(v.x), fmt_float(v.y),
                               fmt_float(v.z), fmt_float(v.w));
        });

    // ---- Quat (scripts use x, y, z, w order, like scene files) ---------------------------------
    lua.new_usertype<Quat>(
        "Quat", sol::no_constructor,
        "new", sol::overload([]() { return Quat(1.0f, 0.0f, 0.0f, 0.0f); },
                             [](f32 x, f32 y, f32 z, f32 w) { return Quat(w, x, y, z); },
                             [](const Quat& q) { return q; }),
        "x", sol::property([](const Quat& q) { return q.x; }, [](Quat& q, f32 s) { q.x = s; }),
        "y", sol::property([](const Quat& q) { return q.y; }, [](Quat& q, f32 s) { q.y = s; }),
        "z", sol::property([](const Quat& q) { return q.z; }, [](Quat& q, f32 s) { q.z = s; }),
        "w", sol::property([](const Quat& q) { return q.w; }, [](Quat& q, f32 s) { q.w = s; }),
        "identity", []() { return Quat(1.0f, 0.0f, 0.0f, 0.0f); },
        // Euler angles in RADIANS (pitch about X, yaw about Y, roll about Z).
        "from_euler", sol::overload([](const Vec3& e) { return Quat(e); },
                                    [](f32 pitch, f32 yaw, f32 roll) { return Quat(Vec3(pitch, yaw, roll)); }),
        "angle_axis", [](f32 angle, const Vec3& axis) { return glm::angleAxis(angle, safe_normalize(axis)); },
        "look_rotation", sol::overload(
            [](const Vec3& forward) { return glm::quatLookAtRH(safe_normalize(forward), Vec3(0, 1, 0)); },
            [](const Vec3& forward, const Vec3& up) {
                return glm::quatLookAtRH(safe_normalize(forward), safe_normalize(up));
            }),
        "slerp", [](const Quat& a, const Quat& b, f32 t) { return glm::slerp(a, b, t); },
        "inverse", [](const Quat& q) { return glm::inverse(q); },
        "normalized", [](const Quat& q) { return glm::normalize(q); },
        "euler", [](const Quat& q) { return glm::eulerAngles(q); },
        "rotate", [](const Quat& q, const Vec3& v) { return q * v; },
        "dot", [](const Quat& a, const Quat& b) { return glm::dot(a, b); },
        sol::meta_function::multiplication,
        sol::overload([](const Quat& a, const Quat& b) { return a * b; },
                      [](const Quat& q, const Vec3& v) { return q * v; }),
        sol::meta_function::equal_to, [](const Quat& a, const Quat& b) { return a == b; },
        sol::meta_function::to_string, [](const Quat& q) {
            return std::format("Quat({}, {}, {}, {})", fmt_float(q.x), fmt_float(q.y),
                               fmt_float(q.z), fmt_float(q.w));
        });

    // ---- Transform (a value: modifying it does not move any entity) ---------------------------
    lua.new_usertype<Transform>(
        "Transform", sol::no_constructor,
        "new", sol::overload([]() { return Transform{}; },
                             [](const Vec3& p) { return Transform{ p, Quat(1, 0, 0, 0), Vec3(1.0f) }; },
                             [](const Vec3& p, const Quat& r) { return Transform{ p, r, Vec3(1.0f) }; },
                             [](const Vec3& p, const Quat& r, const Vec3& s) { return Transform{ p, r, s }; },
                             [](const Transform& t) { return t; }),
        "position", sol::property([](const Transform& t) { return t.position; },
                                  [](Transform& t, const Vec3& v) { t.position = v; }),
        "rotation", sol::property([](const Transform& t) { return t.rotation; },
                                  [](Transform& t, const Quat& q) { t.rotation = q; }),
        "scale", sol::property([](const Transform& t) { return t.scale; },
                               [](Transform& t, const Vec3& v) { t.scale = v; }),
        "transform_point", [](const Transform& t, const Vec3& p) {
            return t.position + t.rotation * (t.scale * p);
        },
        "transform_direction", [](const Transform& t, const Vec3& d) { return t.rotation * d; },
        sol::meta_function::to_string, [](const Transform& t) {
            return std::format("Transform(pos=({}, {}, {}))", fmt_float(t.position.x),
                               fmt_float(t.position.y), fmt_float(t.position.z));
        });

    for (const char* name : { "Vec2", "Vec3", "Vec4", "Quat", "Transform" }) {
        publish_type(impl, name);
    }
    lua_State* L = impl.state();
    harden(L, sol::make_object(L, Vec2(0.0f)));
    harden(L, sol::make_object(L, Vec3(0.0f)));
    harden(L, sol::make_object(L, Vec4(0.0f)));
    harden(L, sol::make_object(L, Quat(1, 0, 0, 0)));
    harden(L, sol::make_object(L, Transform{}));
}

// =================================================================================================
// entities & components
// =================================================================================================
const char* light_kind_name(LightKind k) {
    switch (k) {
    case LightKind::Directional: return "directional";
    case LightKind::Spot: return "spot";
    case LightKind::Point: break;
    }
    return "point";
}

std::optional<LightKind> light_kind_from(std::string_view s) {
    const std::string l = lower(s);
    if (l == "directional") {
        return LightKind::Directional;
    }
    if (l == "point") {
        return LightKind::Point;
    }
    if (l == "spot") {
        return LightKind::Spot;
    }
    return std::nullopt;
}

LightComponent& light_of(lua_State* L, const LightRef& r) {
    const Entity e = checked(L, LuaEntity{ r.id });
    auto*        c = impl_of(L).world.try_get<LightComponent>(e);
    if (c == nullptr) {
        raise_script_error(L, "the entity no longer has a light component");
    }
    return *c;
}

CameraComponent& camera_of(lua_State* L, const CameraRef& r) {
    const Entity e = checked(L, LuaEntity{ r.id });
    auto*        c = impl_of(L).world.try_get<CameraComponent>(e);
    if (c == nullptr) {
        raise_script_error(L, "the entity no longer has a camera component");
    }
    return *c;
}

// World-space rotation of `e`'s parent (identity for roots).
Quat parent_world_rotation(const World& world, Entity e) {
    const Entity parent = scene::parent_of(world, e);
    if (parent == kNullEntity) {
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return scene::decompose_transform(world.world_matrix(parent)).rotation;
}

Vec3 world_axis(const World& world, Entity e, int column, f32 sign) {
    const Mat4 m = world.world_matrix(e);
    return safe_normalize(Vec3(m[column]) * sign);
}

void register_entity(Impl& host) {
    sol::state& lua = host.lua;

    lua.new_usertype<LightRef>(
        "Light", sol::no_constructor,
        "entity", sol::readonly_property([](const LightRef& r, sol::this_state s) {
            return entity_or_nil(s, r.id);
        }),
        "kind", sol::property(
            [](const LightRef& r, sol::this_state s) { return std::string(light_kind_name(light_of(s, r).kind)); },
            [](const LightRef& r, sol::this_state s, std::string_view v) {
                const auto k = light_kind_from(v);
                if (!k) {
                    raise_script_error(s, std::format("unknown light kind '{}' (directional|point|spot)", v));
                }
                light_of(s, r).kind = *k;
            }),
        "color", sol::property([](const LightRef& r, sol::this_state s) { return light_of(s, r).color; },
                               [](const LightRef& r, sol::this_state s, const Vec3& c) { light_of(s, r).color = c; }),
        "intensity", sol::property(
            [](const LightRef& r, sol::this_state s) { return light_of(s, r).intensity; },
            [](const LightRef& r, sol::this_state s, f64 v) {
                light_of(s, r).intensity = finite_or_raise(s, v, "intensity");
            }),
        "range", sol::property(
            [](const LightRef& r, sol::this_state s) { return light_of(s, r).range; },
            [](const LightRef& r, sol::this_state s, f64 v) { light_of(s, r).range = finite_or_raise(s, v, "range"); }),
        "inner_cone", sol::property(
            [](const LightRef& r, sol::this_state s) { return light_of(s, r).inner_cone_deg; },
            [](const LightRef& r, sol::this_state s, f64 v) {
                light_of(s, r).inner_cone_deg = finite_or_raise(s, v, "inner_cone");
            }),
        "outer_cone", sol::property(
            [](const LightRef& r, sol::this_state s) { return light_of(s, r).outer_cone_deg; },
            [](const LightRef& r, sol::this_state s, f64 v) {
                light_of(s, r).outer_cone_deg = finite_or_raise(s, v, "outer_cone");
            }),
        "cast_shadows", sol::property(
            [](const LightRef& r, sol::this_state s) { return light_of(s, r).cast_shadows; },
            [](const LightRef& r, sol::this_state s, bool v) { light_of(s, r).cast_shadows = v; }),
        sol::meta_function::to_string, [](const LightRef& r) {
            return std::format("Light(#{})", entt::to_integral(r.id));
        });

    lua.new_usertype<CameraRef>(
        "Camera", sol::no_constructor,
        "entity", sol::readonly_property([](const CameraRef& r, sol::this_state s) {
            return entity_or_nil(s, r.id);
        }),
        "fov", sol::property(
            [](const CameraRef& r, sol::this_state s) { return camera_of(s, r).fov_y_deg; },
            [](const CameraRef& r, sol::this_state s, f64 v) { camera_of(s, r).fov_y_deg = finite_or_raise(s, v, "fov"); }),
        "near", sol::property(
            [](const CameraRef& r, sol::this_state s) { return camera_of(s, r).near_z; },
            [](const CameraRef& r, sol::this_state s, f64 v) { camera_of(s, r).near_z = finite_or_raise(s, v, "near"); }),
        "far", sol::property(
            [](const CameraRef& r, sol::this_state s) { return camera_of(s, r).far_z; },
            [](const CameraRef& r, sol::this_state s, f64 v) { camera_of(s, r).far_z = finite_or_raise(s, v, "far"); }),
        "primary", sol::property(
            [](const CameraRef& r, sol::this_state s) { return camera_of(s, r).primary; },
            [](const CameraRef& r, sol::this_state s, bool v) { camera_of(s, r).primary = v; }),
        sol::meta_function::to_string, [](const CameraRef& r) {
            return std::format("Camera(#{})", entt::to_integral(r.id));
        });

    lua.new_usertype<LuaEntity>(
        "Entity", sol::no_constructor,
        // ---- identity ------------------------------------------------------------------------
        "valid", [](const LuaEntity& e, sol::this_state s) {
            return e.id != kNullEntity && impl_of(s).world.valid(e.id);
        },
        "id", [](const LuaEntity& e) { return static_cast<i64>(entt::to_integral(e.id)); },
        "uuid", [](const LuaEntity& e, sol::this_state s) {
            return scene::uuid_to_string(scene::uuid_of(impl_of(s).world, checked(s, e)));
        },
        "name", [](const LuaEntity& e, sol::this_state s) {
            const auto* n = impl_of(s).world.try_get<NameComponent>(checked(s, e));
            return n != nullptr ? n->name : std::string();
        },
        "set_name", [](const LuaEntity& e, sol::this_state s, std::string name) {
            World& w = impl_of(s).world;
            const Entity id = checked(s, e);
            if (auto* n = w.try_get<NameComponent>(id)) {
                n->name = std::move(name);
            } else {
                w.add<NameComponent>(id, NameComponent{ std::move(name) });
            }
        },
        "tag", [](const LuaEntity& e, sol::this_state s) {
            const auto* t = impl_of(s).world.try_get<TagComponent>(checked(s, e));
            return t != nullptr ? static_cast<i64>(t->tag) : i64{ 0 };
        },
        "set_tag", [](const LuaEntity& e, sol::this_state s, i64 tag) {
            if (tag < 0 || tag > static_cast<i64>(kInvalidU32)) {
                raise_script_error(s, "tag must be in [0, 2^32)");
            }
            World& w = impl_of(s).world;
            const Entity id = checked(s, e);
            if (auto* t = w.try_get<TagComponent>(id)) {
                t->tag = static_cast<u32>(tag);
            } else {
                w.add<TagComponent>(id, TagComponent{ static_cast<u32>(tag) });
            }
        },

        // ---- local transform -----------------------------------------------------------------
        "position", [](const LuaEntity& e, sol::this_state s) {
            return scene::local_transform(impl_of(s).world, checked(s, e)).position;
        },
        "set_position", [](const LuaEntity& e, sol::this_state s, const Vec3& p) {
            scene::set_local_position(impl_of(s).world, checked(s, e), p);
        },
        "rotation", [](const LuaEntity& e, sol::this_state s) {
            return scene::local_transform(impl_of(s).world, checked(s, e)).rotation;
        },
        "set_rotation", [](const LuaEntity& e, sol::this_state s, const Quat& q) {
            scene::set_local_rotation(impl_of(s).world, checked(s, e), glm::normalize(q));
        },
        "scale", [](const LuaEntity& e, sol::this_state s) {
            return scene::local_transform(impl_of(s).world, checked(s, e)).scale;
        },
        "set_scale", sol::overload(
            [](const LuaEntity& e, sol::this_state s, const Vec3& v) {
                scene::set_local_scale(impl_of(s).world, checked(s, e), v);
            },
            [](const LuaEntity& e, sol::this_state s, f32 v) {
                scene::set_local_scale(impl_of(s).world, checked(s, e), Vec3(v));
            }),
        "transform", [](const LuaEntity& e, sol::this_state s) {
            return scene::local_transform(impl_of(s).world, checked(s, e));
        },
        "set_transform", [](const LuaEntity& e, sol::this_state s, const Transform& t) {
            scene::set_local_transform(impl_of(s).world, checked(s, e), t);
        },
        "translate", [](const LuaEntity& e, sol::this_state s, const Vec3& d) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            scene::set_local_position(w, id, scene::local_transform(w, id).position + d);
        },
        // Applies `q` in the entity's local frame (rotation = rotation * q).
        "rotate", [](const LuaEntity& e, sol::this_state s, const Quat& q) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            scene::set_local_rotation(w, id, glm::normalize(scene::local_transform(w, id).rotation * q));
        },

        // ---- world space ---------------------------------------------------------------------
        "world_position", [](const LuaEntity& e, sol::this_state s) {
            return scene::world_position(impl_of(s).world, checked(s, e));
        },
        "set_world_position", [](const LuaEntity& e, sol::this_state s, const Vec3& p) {
            scene::set_world_position(impl_of(s).world, checked(s, e), p);
        },
        "world_rotation", [](const LuaEntity& e, sol::this_state s) {
            const World& w = impl_of(s).world;
            return scene::decompose_transform(w.world_matrix(checked(s, e))).rotation;
        },
        "set_world_rotation", [](const LuaEntity& e, sol::this_state s, const Quat& q) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            scene::set_local_rotation(w, id, glm::normalize(glm::inverse(parent_world_rotation(w, id)) * q));
        },
        "forward", [](const LuaEntity& e, sol::this_state s) {
            return world_axis(impl_of(s).world, checked(s, e), 2, -1.0f);
        },
        "right", [](const LuaEntity& e, sol::this_state s) {
            return world_axis(impl_of(s).world, checked(s, e), 0, 1.0f);
        },
        "up", [](const LuaEntity& e, sol::this_state s) {
            return world_axis(impl_of(s).world, checked(s, e), 1, 1.0f);
        },
        // Rotates the entity so its forward (-Z) points at a world-space position.
        "look_at", sol::overload(
            [](const LuaEntity& e, sol::this_state s, const Vec3& target) {
                World&       w   = impl_of(s).world;
                const Entity id  = checked(s, e);
                const Vec3   dir = target - scene::world_position(w, id);
                if (glm::dot(dir, dir) < 1e-12f) {
                    return;
                }
                const Vec3 fwd = glm::normalize(dir);
                const Vec3 up  = std::abs(fwd.y) > 0.999f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
                const Quat q   = glm::quatLookAtRH(fwd, up);
                scene::set_local_rotation(w, id, glm::normalize(glm::inverse(parent_world_rotation(w, id)) * q));
            },
            [](const LuaEntity& e, sol::this_state s, const LuaEntity& target) {
                World&       w   = impl_of(s).world;
                const Entity id  = checked(s, e);
                const Vec3   dir = scene::world_position(w, checked(s, target)) - scene::world_position(w, id);
                if (glm::dot(dir, dir) < 1e-12f) {
                    return;
                }
                const Vec3 fwd = glm::normalize(dir);
                const Vec3 up  = std::abs(fwd.y) > 0.999f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
                const Quat q   = glm::quatLookAtRH(fwd, up);
                scene::set_local_rotation(w, id, glm::normalize(glm::inverse(parent_world_rotation(w, id)) * q));
            }),

        // ---- hierarchy -----------------------------------------------------------------------
        "parent", [](const LuaEntity& e, sol::this_state s) {
            return entity_or_nil(s, scene::parent_of(impl_of(s).world, checked(s, e)));
        },
        // set_parent(parent|nil [, keep_world = true]); returns false if rejected (cycles).
        "set_parent", [](const LuaEntity& e, sol::this_state s, const sol::object& parent,
                         sol::optional<bool> keep_world) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            Entity       p  = kNullEntity;
            if (parent.valid() && parent.get_type() != sol::type::lua_nil) {
                const auto pe = entity_from_lua(parent);
                if (!pe) {
                    raise_script_error(s, "set_parent expects an Entity or nil");
                }
                p = checked(s, LuaEntity{ *pe });
            }
            if (keep_world.value_or(true)) {
                return scene::set_parent_keep_world(w, id, p);
            }
            if (p != kNullEntity && !scene::can_set_parent(w, id, p)) {
                return false;
            }
            w.set_parent(id, p);
            return true;
        },
        "children", [](const LuaEntity& e, sol::this_state s) {
            Impl&        impl = impl_of(s);
            const Entity id   = checked(s, e);
            sol::table   out  = sol::state_view(s).create_table();
            int          i    = 1;
            scene::for_each_child(impl.world, id, [&](Entity c) { out.raw_set(i++, impl.entity_object(c, s)); });
            return out;
        },
        "child", [](const LuaEntity& e, sol::this_state s, std::string_view name) {
            const World& w = impl_of(s).world;
            return entity_or_nil(s, scene::find_child_by_name(w, checked(s, e), name));
        },
        "find", [](const LuaEntity& e, sol::this_state s, std::string_view path) {
            const World& w = impl_of(s).world;
            return entity_or_nil(s, scene::find_by_path(w, checked(s, e), path));
        },

        // ---- visibility ----------------------------------------------------------------------
        "visible", [](const LuaEntity& e, sol::this_state s) {
            return scene::is_visible(impl_of(s).world, checked(s, e));
        },
        "set_visible", [](const LuaEntity& e, sol::this_state s, bool v) {
            scene::set_visible(impl_of(s).world, checked(s, e), v);
        },

        // ---- components ----------------------------------------------------------------------
        "light", [](const LuaEntity& e, sol::this_state s) -> sol::object {
            const Entity id = checked(s, e);
            if (!impl_of(s).world.has<LightComponent>(id)) {
                return sol::make_object(s, sol::lua_nil);
            }
            return sol::make_object(s, LightRef{ id });
        },
        "add_light", [](const LuaEntity& e, sol::this_state s, sol::optional<std::string_view> kind) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            LightComponent c;
            if (kind) {
                const auto k = light_kind_from(*kind);
                if (!k) {
                    raise_script_error(s, std::format("unknown light kind '{}' (directional|point|spot)", *kind));
                }
                c.kind = *k;
            }
            if (!w.has<LightComponent>(id)) {
                w.add<LightComponent>(id, c);
            }
            return LightRef{ id };
        },
        "remove_light", [](const LuaEntity& e, sol::this_state s) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            if (w.has<LightComponent>(id)) {
                w.remove<LightComponent>(id);
            }
        },
        "camera", [](const LuaEntity& e, sol::this_state s) -> sol::object {
            const Entity id = checked(s, e);
            if (!impl_of(s).world.has<CameraComponent>(id)) {
                return sol::make_object(s, sol::lua_nil);
            }
            return sol::make_object(s, CameraRef{ id });
        },
        "add_camera", [](const LuaEntity& e, sol::this_state s) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            if (!w.has<CameraComponent>(id)) {
                w.add<CameraComponent>(id);
            }
            return CameraRef{ id };
        },
        "remove_camera", [](const LuaEntity& e, sol::this_state s) {
            World&       w  = impl_of(s).world;
            const Entity id = checked(s, e);
            if (w.has<CameraComponent>(id)) {
                w.remove<CameraComponent>(id);
            }
        },

        // ---- scripts -------------------------------------------------------------------------
        // The `self` table of the entity's script instance (nil without one).
        "script", [](const LuaEntity& e, sol::this_state s) -> sol::object {
            Impl&           impl = impl_of(s);
            const Instance* inst = impl.find_instance(checked(s, e));
            if (inst == nullptr) {
                return sol::make_object(s, sol::lua_nil);
            }
            return sol::object(inst->self);
        },
        "has_script", [](const LuaEntity& e, sol::this_state s) {
            return impl_of(s).world.has<ScriptComponent>(checked(s, e));
        },
        // add_script(path [, properties]) — the instance starts at the next update.
        "add_script", [](const LuaEntity& e, sol::this_state s, std::string path,
                         sol::optional<sol::table> props) {
            Impl&        impl = impl_of(s);
            const Entity id   = checked(s, e);
            std::string  normalized;
            std::string  err;
            if (!normalize_script_path(path, normalized, err)) {
                raise_script_error(s, err);
            }
            ScriptComponent c;
            c.script = std::move(path);
            if (props) {
                props->for_each([&](const sol::object& k, const sol::object& v) {
                    if (k.get_type() != sol::type::string) {
                        return;
                    }
                    if (auto value = Impl::from_lua(v)) {
                        c.set_property(k.as<std::string>(), std::move(*value));
                    }
                });
            }
            impl.world.add<ScriptComponent>(id, std::move(c));
            if (!impl.config.auto_attach) {
                impl.pending_attach.push_back(id);
            }
        },
        "remove_script", [](const LuaEntity& e, sol::this_state s) {
            Impl&        impl = impl_of(s);
            const Entity id   = checked(s, e);
            if (impl.world.has<ScriptComponent>(id)) {
                impl.world.remove<ScriptComponent>(id);
                if (!impl.config.auto_attach) {
                    impl.pending_detach.push_back(id);
                }
            }
        },

        // ---- lifetime ------------------------------------------------------------------------
        // Destroys the entity and its subtree after the current callbacks finish.
        "destroy", [](const LuaEntity& e, sol::this_state s) {
            Impl& impl = impl_of(s);
            if (e.id != kNullEntity && impl.world.valid(e.id)) {
                impl.pending_destroy.push_back(e.id);
            }
        },

        sol::meta_function::equal_to, [](const LuaEntity& a, const LuaEntity& b) { return a.id == b.id; },
        sol::meta_function::to_string, [](const LuaEntity& e, sol::this_state s) {
            const World& w  = impl_of(s).world;
            const auto   id = entt::to_integral(e.id);
            if (e.id == kNullEntity || !w.valid(e.id)) {
                return std::format("Entity(#{} destroyed)", id);
            }
            const auto* n = w.try_get<NameComponent>(e.id);
            return std::format("Entity(#{} '{}')", id, n != nullptr ? n->name : std::string());
        });

    lua_State* L = host.state();
    harden(L, sol::make_object(L, LightRef{}));
    harden(L, sol::make_object(L, CameraRef{}));
    harden(L, sol::make_object(L, LuaEntity{}));
    lua.set("Light", sol::lua_nil);
    lua.set("Camera", sol::lua_nil);
    lua.set("Entity", sol::lua_nil);

    // ---- world -------------------------------------------------------------------------------
    sol::table world = lua.create_table();
    world.set_function("spawn", [](sol::this_state s, sol::optional<std::string> name,
                                   const sol::object& parent) {
        Impl&  impl = impl_of(s);
        Entity p    = kNullEntity;
        if (parent.valid() && parent.get_type() != sol::type::lua_nil) {
            const auto pe = entity_from_lua(parent);
            if (!pe) {
                raise_script_error(s, "world.spawn: parent must be an Entity or nil");
            }
            p = checked(s, LuaEntity{ *pe });
        }
        const std::string n = name.value_or("Entity");
        const Entity      e = p != kNullEntity ? impl.world.create_child(p, n) : impl.world.create(n);
        return impl.entity_object(e, s);
    });
    world.set_function("find", [](sol::this_state s, std::string_view name) {
        return entity_or_nil(s, scene::find_by_name(impl_of(s).world, name));
    });
    world.set_function("find_path", [](sol::this_state s, std::string_view path) {
        return entity_or_nil(s, scene::find_by_path(impl_of(s).world, path));
    });
    world.set_function("find_uuid", [](sol::this_state s, std::string_view text) {
        u64 uuid = 0;
        if (!scene::uuid_from_string(text, uuid)) {
            return sol::make_object(s, sol::lua_nil);
        }
        return entity_or_nil(s, scene::find_by_uuid(impl_of(s).world, uuid));
    });
    world.set_function("destroy", [](sol::this_state s, const sol::object& target) {
        const auto e = entity_from_lua(target);
        if (!e) {
            raise_script_error(s, "world.destroy expects an Entity");
        }
        Impl& impl = impl_of(s);
        if (*e != kNullEntity && impl.world.valid(*e)) {
            impl.pending_destroy.push_back(*e);
        }
    });
    world.set_function("count", [](sol::this_state s) {
        return static_cast<i64>(impl_of(s).world.entity_count());
    });
    world.set_function("roots", [](sol::this_state s) {
        Impl&      impl = impl_of(s);
        sol::table out  = sol::state_view(s).create_table();
        int        i    = 1;
        scene::for_each_root(impl.world, [&](Entity r) { out.raw_set(i++, impl.entity_object(r, s)); });
        return out;
    });
    host.base.raw_set("world", host.freeze(world));
}

// =================================================================================================
// input
// =================================================================================================
struct KeyName {
    std::string_view name;
    Key              key;
};

constexpr std::array kNamedKeys{
    KeyName{ "space", Key::Space },           KeyName{ "apostrophe", Key::Apostrophe },
    KeyName{ "comma", Key::Comma },           KeyName{ "minus", Key::Minus },
    KeyName{ "period", Key::Period },         KeyName{ "slash", Key::Slash },
    KeyName{ "semicolon", Key::Semicolon },   KeyName{ "equal", Key::Equal },
    KeyName{ "left_bracket", Key::LeftBracket }, KeyName{ "backslash", Key::Backslash },
    KeyName{ "right_bracket", Key::RightBracket }, KeyName{ "grave", Key::GraveAccent },
    KeyName{ "escape", Key::Escape },         KeyName{ "enter", Key::Enter },
    KeyName{ "tab", Key::Tab },               KeyName{ "backspace", Key::Backspace },
    KeyName{ "insert", Key::Insert },         KeyName{ "delete", Key::Delete },
    KeyName{ "right", Key::Right },           KeyName{ "left", Key::Left },
    KeyName{ "down", Key::Down },             KeyName{ "up", Key::Up },
    KeyName{ "page_up", Key::PageUp },        KeyName{ "page_down", Key::PageDown },
    KeyName{ "home", Key::Home },             KeyName{ "end", Key::End },
    KeyName{ "caps_lock", Key::CapsLock },    KeyName{ "left_shift", Key::LeftShift },
    KeyName{ "left_ctrl", Key::LeftControl }, KeyName{ "left_alt", Key::LeftAlt },
    KeyName{ "left_super", Key::LeftSuper },  KeyName{ "right_shift", Key::RightShift },
    KeyName{ "right_ctrl", Key::RightControl }, KeyName{ "right_alt", Key::RightAlt },
    KeyName{ "right_super", Key::RightSuper },
};

// Up to two physical keys for a key name ("shift"/"ctrl"/"alt"/"super" match either side).
struct KeySet {
    Key keys[2] = { Key::Unknown, Key::Unknown };
};

std::optional<KeySet> keys_from_name(std::string_view raw) {
    const std::string n = lower(raw);
    if (n.size() == 1) {
        const char c = n[0];
        if (c >= 'a' && c <= 'z') {
            return KeySet{ { static_cast<Key>(static_cast<u16>(Key::A) + static_cast<u16>(c - 'a')), Key::Unknown } };
        }
        if (c >= '0' && c <= '9') {
            return KeySet{ { static_cast<Key>(static_cast<u16>(Key::Num0) + static_cast<u16>(c - '0')), Key::Unknown } };
        }
    }
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f') {
        int num = 0;
        for (usize i = 1; i < n.size(); ++i) {
            if (n[i] < '0' || n[i] > '9') {
                num = -1;
                break;
            }
            num = num * 10 + (n[i] - '0');
        }
        if (num >= 1 && num <= 12) {
            return KeySet{ { static_cast<Key>(static_cast<u16>(Key::F1) + static_cast<u16>(num - 1)), Key::Unknown } };
        }
    }
    if (n == "shift") {
        return KeySet{ { Key::LeftShift, Key::RightShift } };
    }
    if (n == "ctrl" || n == "control") {
        return KeySet{ { Key::LeftControl, Key::RightControl } };
    }
    if (n == "alt") {
        return KeySet{ { Key::LeftAlt, Key::RightAlt } };
    }
    if (n == "super") {
        return KeySet{ { Key::LeftSuper, Key::RightSuper } };
    }
    for (const KeyName& k : kNamedKeys) {
        if (k.name == n) {
            return KeySet{ { k.key, Key::Unknown } };
        }
    }
    return std::nullopt;
}

KeySet checked_keys(lua_State* L, std::string_view name) {
    const auto ks = keys_from_name(name);
    if (!ks) {
        raise_script_error(L, std::format("unknown key name '{}'", name));
    }
    return *ks;
}

MouseButton checked_button(lua_State* L, std::string_view raw) {
    const std::string n = lower(raw);
    if (n == "left") {
        return MouseButton::Left;
    }
    if (n == "right") {
        return MouseButton::Right;
    }
    if (n == "middle") {
        return MouseButton::Middle;
    }
    if (n == "button4") {
        return MouseButton::Button4;
    }
    if (n == "button5") {
        return MouseButton::Button5;
    }
    raise_script_error(L, std::format("unknown mouse button '{}' (left|right|middle|button4|button5)", raw));
}

template <typename Pred>
bool any_key(const KeySet& ks, Pred&& pred) {
    for (const Key k : ks.keys) {
        if (k != Key::Unknown && pred(k)) {
            return true;
        }
    }
    return false;
}

ButtonState key_state(const InputState& in, Key k) { return in.keys[static_cast<usize>(k)]; }
ButtonState button_state(const InputState& in, MouseButton b) { return in.mouse[static_cast<usize>(b)]; }

void register_input(Impl& host) {
    sol::table input = host.lua.create_table();
    input.set_function("available", [](sol::this_state s) { return impl_of(s).input != nullptr; });
    input.set_function("key_down", [](sol::this_state s, std::string_view name) {
        const KeySet       ks = checked_keys(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && any_key(ks, [&](Key k) { return in->key_down(k); });
    });
    input.set_function("key_pressed", [](sol::this_state s, std::string_view name) {
        const KeySet       ks = checked_keys(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && any_key(ks, [&](Key k) { return key_state(*in, k) == ButtonState::Pressed; });
    });
    input.set_function("key_released", [](sol::this_state s, std::string_view name) {
        const KeySet       ks = checked_keys(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && any_key(ks, [&](Key k) { return key_state(*in, k) == ButtonState::Released; });
    });
    input.set_function("mouse_down", [](sol::this_state s, std::string_view name) {
        const MouseButton  b  = checked_button(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && in->mouse_down(b);
    });
    input.set_function("mouse_pressed", [](sol::this_state s, std::string_view name) {
        const MouseButton  b  = checked_button(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && button_state(*in, b) == ButtonState::Pressed;
    });
    input.set_function("mouse_released", [](sol::this_state s, std::string_view name) {
        const MouseButton  b  = checked_button(s, name);
        const InputState* in = impl_of(s).input;
        return in != nullptr && button_state(*in, b) == ButtonState::Released;
    });
    input.set_function("mouse_position", [](sol::this_state s) {
        const InputState* in = impl_of(s).input;
        return in != nullptr ? in->cursor : Vec2(0.0f);
    });
    input.set_function("mouse_delta", [](sol::this_state s) {
        const InputState* in = impl_of(s).input;
        return in != nullptr ? in->cursor_delta : Vec2(0.0f);
    });
    input.set_function("scroll", [](sol::this_state s) {
        const InputState* in = impl_of(s).input;
        return in != nullptr ? in->scroll : Vec2(0.0f);
    });
    // Signed axis from two keys: axis("d", "a") -> 1 / -1 / 0.
    input.set_function("axis", [](sol::this_state s, std::string_view positive, std::string_view negative) {
        const KeySet       pos = checked_keys(s, positive);
        const KeySet       neg = checked_keys(s, negative);
        const InputState* in  = impl_of(s).input;
        if (in == nullptr) {
            return 0.0;
        }
        const bool p = any_key(pos, [&](Key k) { return in->key_down(k); });
        const bool n = any_key(neg, [&](Key k) { return in->key_down(k); });
        return (p ? 1.0 : 0.0) - (n ? 1.0 : 0.0);
    });
    host.base.raw_set("input", host.freeze(input));
}

// =================================================================================================
// time & timers
// =================================================================================================
void register_time(Impl& host) {
    sol::state& lua = host.lua;
    lua.new_usertype<TimeState>(
        "TimeState", sol::no_constructor,
        "time", sol::readonly_property([](const TimeState& t) { return t.time; }),
        "fixed_time", sol::readonly_property([](const TimeState& t) { return t.fixed_time; }),
        "dt", sol::readonly_property([](const TimeState& t) { return t.dt; }),
        "frame_dt", sol::readonly_property([](const TimeState& t) { return t.frame_dt; }),
        "fixed_dt", sol::readonly_property([](const TimeState& t) { return t.fixed_dt; }),
        "frame", sol::readonly_property([](const TimeState& t) { return static_cast<i64>(t.frame); }),
        sol::meta_function::to_string, [](const TimeState& t) {
            return std::format("time(t={:.3f}, frame={})", t.time, t.frame);
        });
    lua.set("TimeState", sol::lua_nil);
    host.base.raw_set("time", &host.time);
    lua_State* L = host.state();
    harden(L, sol::make_object(L, &host.time));

    lua.new_usertype<TimerHandle>(
        "TimerHandle", sol::no_constructor,
        "cancel", [](const TimerHandle& h, sol::this_state s) { impl_of(s).cancel_timer(h.id); },
        "active", [](const TimerHandle& h, sol::this_state s) {
            const Timer* t = impl_of(s).find_timer(h.id);
            return t != nullptr && !t->cancelled;
        },
        sol::meta_function::equal_to, [](const TimerHandle& a, const TimerHandle& b) { return a.id == b.id; },
        sol::meta_function::to_string, [](const TimerHandle& h) { return std::format("Timer({})", h.id); });
    lua.set("TimerHandle", sol::lua_nil);
    harden(L, sol::make_object(L, TimerHandle{}));

    sol::table timer = lua.create_table();
    // timer.after(seconds, fn(handle)) — fires once, `seconds` of script time from now.
    timer.set_function("after", [](sol::this_state s, f64 delay, const sol::object& fn) {
        if (!std::isfinite(delay) || delay < 0.0) {
            raise_script_error(s, "timer.after: delay must be a finite number >= 0");
        }
        if (fn.get_type() != sol::type::function) {
            raise_script_error(s, "timer.after: expected a function");
        }
        return impl_of(s).add_timer(delay, false, to_main(fn));
    });
    // timer.every(seconds, fn(handle)) — repeats until cancelled (at most once per update).
    timer.set_function("every", [](sol::this_state s, f64 interval, const sol::object& fn) {
        if (!std::isfinite(interval) || interval <= 0.0) {
            raise_script_error(s, "timer.every: interval must be a finite number > 0");
        }
        if (fn.get_type() != sol::type::function) {
            raise_script_error(s, "timer.every: expected a function");
        }
        return impl_of(s).add_timer(interval, true, to_main(fn));
    });
    timer.set_function("cancel", [](sol::this_state s, const TimerHandle& h) { impl_of(s).cancel_timer(h.id); });
    host.base.raw_set("timer", host.freeze(timer));
}

} // namespace

void register_bindings(ScriptVM::Impl& impl) {
    register_math(impl);
    register_entity(impl);
    register_input(impl);
    register_time(impl);
}

} // namespace aether::scripting
