// components.cpp — the "Script" ComponentCodec (JSON via nlohmann, private to this TU).
//
// Format:
//   "Script": { "script": "ai/patrol.lua", "enabled": true,
//               "properties": [ { "name": "speed", "value": 2.5 }, ... ] }
// Property values: null | bool | integer | number | string, or a tagged object for the
// non-JSON types: {"vec2": [x,y]}, {"vec3": [x,y,z]}, {"vec4": [x,y,z,w]},
// {"quat": [x,y,z,w]}, {"entity": "<uuid hex>"} (an empty uuid string is the null entity).
// Properties are an array so their authoring order survives a round trip.
#include "aether/scripting/components.h"

#include "aether/core/log.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <nlohmann/json.hpp>

#include <string>
#include <type_traits>

namespace aether::scripting {

namespace {

using json = nlohmann::json;

constexpr StringView kCodecName = "Script";

template <typename V, int N>
json vec_to_json(const V& v) {
    json a = json::array();
    for (int i = 0; i < N; ++i) {
        a.push_back(static_cast<f64>(v[i]));
    }
    return a;
}

template <typename V, int N>
bool vec_from_json(const json& j, V& out) {
    if (!j.is_array() || j.size() != static_cast<usize>(N)) {
        return false;
    }
    for (int i = 0; i < N; ++i) {
        if (!j[static_cast<usize>(i)].is_number()) {
            return false;
        }
        out[i] = j[static_cast<usize>(i)].get<f32>();
    }
    return true;
}

json value_to_json(const World& world, const ScriptValue& value) {
    return std::visit(
        [&](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, i64> ||
                                 std::is_same_v<T, f64> || std::is_same_v<T, std::string>) {
                return json(x);
            } else if constexpr (std::is_same_v<T, Vec2>) {
                return json{ { "vec2", vec_to_json<Vec2, 2>(x) } };
            } else if constexpr (std::is_same_v<T, Vec3>) {
                return json{ { "vec3", vec_to_json<Vec3, 3>(x) } };
            } else if constexpr (std::is_same_v<T, Vec4>) {
                return json{ { "vec4", vec_to_json<Vec4, 4>(x) } };
            } else if constexpr (std::is_same_v<T, Quat>) {
                return json{ { "quat", json::array({ x.x, x.y, x.z, x.w }) } };
            } else {
                static_assert(std::is_same_v<T, Entity>);
                const u64 uuid = scene::uuid_of(world, x);
                return json{ { "entity", uuid != scene::kInvalidUuid ? scene::uuid_to_string(uuid)
                                                                     : std::string() } };
            }
        },
        value);
}

// Returns false for malformed values (the property is skipped with a warning).
bool value_from_json(const World& world, const json& j, ScriptValue& out) {
    switch (j.type()) {
    case json::value_t::null: out = std::monostate{}; return true;
    case json::value_t::boolean: out = j.get<bool>(); return true;
    case json::value_t::number_integer: out = j.get<i64>(); return true;
    case json::value_t::number_unsigned: out = static_cast<i64>(j.get<u64>()); return true;
    case json::value_t::number_float: out = j.get<f64>(); return true;
    case json::value_t::string: out = j.get<std::string>(); return true;
    case json::value_t::object: {
        if (j.size() != 1) {
            return false;
        }
        const auto         first = j.begin(); // (not items(): its proxy values are temporaries)
        const std::string& tag   = first.key();
        const json&        v     = first.value();
        if (tag == "vec2") {
            Vec2 r(0.0f);
            return vec_from_json<Vec2, 2>(v, r) ? (out = r, true) : false;
        }
        if (tag == "vec3") {
            Vec3 r(0.0f);
            return vec_from_json<Vec3, 3>(v, r) ? (out = r, true) : false;
        }
        if (tag == "vec4") {
            Vec4 r(0.0f);
            return vec_from_json<Vec4, 4>(v, r) ? (out = r, true) : false;
        }
        if (tag == "quat") {
            Vec4 r(0.0f);
            if (!vec_from_json<Vec4, 4>(v, r)) {
                return false;
            }
            out = Quat(r.w, r.x, r.y, r.z);
            return true;
        }
        if (tag == "entity") {
            if (!v.is_string()) {
                return false;
            }
            const std::string text = v.get<std::string>();
            u64               uuid = 0;
            if (text.empty()) {
                out = Entity{ kNullEntity };
                return true;
            }
            if (!scene::uuid_from_string(text, uuid)) {
                return false;
            }
            // A reference outside the loaded document (or to a re-keyed entity) resolves to null.
            out = Entity{ scene::find_by_uuid(world, uuid) };
            return true;
        }
        return false;
    }
    default: return false;
    }
}

bool save_script(const World& world, Entity e, std::string& out) {
    const auto* c = world.try_get<ScriptComponent>(e);
    if (c == nullptr) {
        return false;
    }
    json props = json::array();
    for (const ScriptProperty& p : c->properties) {
        props.push_back(json{ { "name", p.name }, { "value", value_to_json(world, p.value) } });
    }
    const json j = { { "script", c->script }, { "enabled", c->enabled }, { "properties", std::move(props) } };
    out          = j.dump();
    return true;
}

Result<void> load_script(World& world, Entity e, StringView text) {
    const json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "Script: expected a JSON object");
    }
    ScriptComponent c;
    if (const auto it = j.find("script"); it != j.end()) {
        if (!it->is_string()) {
            return make_error(ErrorCode::InvalidArgument, "Script: 'script' must be a string");
        }
        c.script = it->get<std::string>();
    }
    if (const auto it = j.find("enabled"); it != j.end() && it->is_boolean()) {
        c.enabled = it->get<bool>();
    }
    if (const auto it = j.find("properties"); it != j.end()) {
        if (!it->is_array()) {
            return make_error(ErrorCode::InvalidArgument, "Script: 'properties' must be an array");
        }
        for (const json& p : *it) {
            const auto name = p.is_object() ? p.find("name") : p.end();
            if (!p.is_object() || name == p.end() || !name->is_string()) {
                AE_LOG_WARN("Script", "Script codec: skipping a property without a name");
                continue;
            }
            ScriptValue value;
            const auto  v = p.find("value");
            if (v != p.end() && !value_from_json(world, *v, value)) {
                AE_LOG_WARN("Script", "Script codec: skipping malformed property '{}'",
                            name->get<std::string>());
                continue;
            }
            c.set_property(name->get<std::string>(), std::move(value));
        }
    }
    // emplace_or_replace fires on_construct/on_update, so a live ScriptVM (re)attaches.
    world.add<ScriptComponent>(e, std::move(c));
    return {};
}

} // namespace

bool register_script_component_codec(World& world) {
    scene::ComponentCodec codec;
    codec.name = std::string(kCodecName);
    codec.save = &save_script;
    codec.load = &load_script;
    return scene::register_component_codec(world, std::move(codec));
}

} // namespace aether::scripting
