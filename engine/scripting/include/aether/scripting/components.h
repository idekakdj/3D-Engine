// aether/scripting/components.h — ECS components and value types of the scripting module.
//
// ScriptComponent attaches a Lua script (path relative to content/scripts) to an entity. All
// runtime state (the script's `self` table, timers, event subscriptions) lives in the World's
// ScriptVM keyed by entity; the component only carries authoring data, so it is trivially
// copyable, serializable (see register_script_component_codec) and never exposes Lua types.
//
// Thread-affinity: plain data; follow the World's rules (main thread for mutation).
#pragma once

#include "aether/core/math.h"
#include "aether/core/reflect.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aether {
class World;
} // namespace aether

namespace aether::scripting {

// A Lua-representable value that crosses the C++/Lua boundary without exposing Lua types:
// nil (monostate), boolean, integer, number, string, math types and entity references.
using ScriptValue =
    std::variant<std::monostate, bool, i64, f64, std::string, Vec2, Vec3, Vec4, Quat, Entity>;

// A named value: a per-component property override, or an event payload field.
struct ScriptProperty {
    std::string name;
    ScriptValue value;
};

struct ScriptComponent {
    // Script path relative to content/scripts, e.g. "rotator.lua" or "ai/patrol.lua".
    std::string script;
    // Disabled components keep their instance (and self table) but receive no callbacks.
    bool enabled = true;
    // Overrides of the script's `properties = { ... }` defaults, copied onto `self`.
    std::vector<ScriptProperty> properties;
    // Opaque: the ScriptVM's instance id for this component. Owned by the VM; do not edit.
    u32 runtime_id = kInvalidU32;

    void set_property(std::string_view name, ScriptValue value) {
        for (ScriptProperty& p : properties) {
            if (p.name == name) {
                p.value = std::move(value);
                return;
            }
        }
        properties.push_back(ScriptProperty{ std::string(name), std::move(value) });
    }

    [[nodiscard]] const ScriptValue* find_property(std::string_view name) const {
        for (const ScriptProperty& p : properties) {
            if (p.name == name) {
                return &p.value;
            }
        }
        return nullptr;
    }

    bool remove_property(std::string_view name) {
        const auto it = std::find_if(properties.begin(), properties.end(),
                                     [&](const ScriptProperty& p) { return p.name == name; });
        if (it == properties.end()) {
            return false;
        }
        properties.erase(it);
        return true;
    }

    AE_REFLECT(ScriptComponent, AE_FIELD(script), AE_FIELD(enabled), AE_FIELD(properties))
};

// Registers the "Script" ComponentCodec (scene_serializer.h) on `world` so ScriptComponents
// survive .aescene save/load, copy/paste and play-in-editor snapshots. Idempotent. The
// ScriptVM constructor calls this; tools that serialize worlds without a VM may call it
// directly. Entity-valued properties are stored as uuids. Main thread only.
bool register_script_component_codec(World& world);

} // namespace aether::scripting
