// component_codecs.cpp — JSON codecs for physics and gameplay components (see
// component_codecs.h). Values missing from a document keep the component's defaults.
#include "aether/gameplay/component_codecs.h"

#include "aether/core/log.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/components.h"
#include "aether/physics/components.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/serialization.h"
#endif
#if AE_WITH_SCRIPTING
#    include "aether/scripting/components.h"
#endif

#include <nlohmann/json.hpp>

#include <string>
#include <type_traits>

namespace aether::gameplay {

namespace {

using json = nlohmann::json;
using namespace aether::physics;

// ---- value helpers ------------------------------------------------------------------------------
json to_json(const Vec3& v) { return json::array({ v.x, v.y, v.z }); }
json to_json(const Quat& q) { return json::array({ q.x, q.y, q.z, q.w }); }

bool read(const json& j, const char* key, Vec3& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_array() || it->size() != 3 || !(*it)[0].is_number() || !(*it)[1].is_number() ||
        !(*it)[2].is_number()) {
        return false;
    }
    out = Vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
    return true;
}

bool read(const json& j, const char* key, Quat& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_array() || it->size() != 4) {
        return false;
    }
    for (const json& c : *it) {
        if (!c.is_number()) {
            return false;
        }
    }
    out = glm::normalize(Quat((*it)[3].get<f32>(), (*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>()));
    return true;
}

template <typename T>
bool read(const json& j, const char* key, T& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            return false;
        }
    } else if constexpr (std::is_arithmetic_v<T>) {
        if (!it->is_number()) {
            return false;
        }
    }
    out = it->get<T>();
    return true;
}

json entity_ref(const World& world, Entity e) {
    const u64 uuid = e != kNullEntity && world.valid(e) ? scene::uuid_of(world, e) : scene::kInvalidUuid;
    return uuid != scene::kInvalidUuid ? scene::uuid_to_string(uuid) : std::string();
}

bool read_entity(const World& world, const json& j, const char* key, Entity& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    const std::string text = it->get<std::string>();
    if (text.empty()) {
        out = kNullEntity;
        return true;
    }
    u64 uuid = 0;
    if (!scene::uuid_from_string(text, uuid)) {
        return false;
    }
    out = scene::find_by_uuid(world, uuid); // references outside the document resolve to null
    return true;
}

Result<json> parse_object(StringView text, const char* what) {
    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        return Error{ ErrorCode::InvalidArgument, std::string(what) + ": expected a JSON object" };
    }
    return j;
}

Result<void> malformed(const char* what) {
    return make_error(ErrorCode::InvalidArgument, std::string(what) + ": malformed field");
}

// Registers a codec from typed save/load functions.
template <typename T, typename Save, typename Load>
bool add_codec(World& world, const char* name, Save save, Load load) {
    scene::ComponentCodec codec;
    codec.name = name;
    codec.save = [save](const World& w, Entity e, std::string& out) {
        const T* c = w.try_get<T>(e);
        if (c == nullptr) {
            return false;
        }
        out = save(w, *c).dump();
        return true;
    };
    codec.load = [load, name](World& w, Entity e, StringView text) -> Result<void> {
        auto j = parse_object(text, name);
        if (!j) {
            return j.error();
        }
        T c{};
        if (const T* existing = w.try_get<T>(e)) {
            c = *existing;
        }
        if (!load(w, *j, c)) {
            return malformed(name);
        }
        w.add<T>(e, std::move(c)); // emplace_or_replace: physics sees the change via signals
        return {};
    };
    return scene::register_component_codec(world, std::move(codec));
}

const char* motion_name(MotionType m) {
    switch (m) {
    case MotionType::Static: return "static";
    case MotionType::Kinematic: return "kinematic";
    case MotionType::Dynamic: break;
    }
    return "dynamic";
}

bool motion_from(const json& j, MotionType& out) {
    const auto it = j.find("motion");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    const std::string s = it->get<std::string>();
    if (s == "static") {
        out = MotionType::Static;
    } else if (s == "kinematic") {
        out = MotionType::Kinematic;
    } else if (s == "dynamic") {
        out = MotionType::Dynamic;
    } else {
        return false;
    }
    return true;
}

const char* shape_name(ColliderShape s) {
    switch (s) {
    case ColliderShape::Box: return "box";
    case ColliderShape::Sphere: return "sphere";
    case ColliderShape::Capsule: return "capsule";
    case ColliderShape::Cylinder: return "cylinder";
    case ColliderShape::ConvexHull: return "convex_hull";
    case ColliderShape::TriangleMesh: return "triangle_mesh";
    }
    return "box";
}

bool shape_from(const json& j, ColliderShape& out) {
    const auto it = j.find("shape");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    const std::string s = it->get<std::string>();
    for (u8 i = 0; i <= static_cast<u8>(ColliderShape::TriangleMesh); ++i) {
        if (s == shape_name(static_cast<ColliderShape>(i))) {
            out = static_cast<ColliderShape>(i);
            return true;
        }
    }
    return false;
}

json points_to_json(const std::vector<Vec3>& pts) {
    json a = json::array();
    for (const Vec3& p : pts) {
        a.push_back(p.x);
        a.push_back(p.y);
        a.push_back(p.z);
    }
    return a; // flat [x0, y0, z0, x1, ...]: compact for large meshes
}

bool points_from(const json& j, const char* key, std::vector<Vec3>& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_array() || it->size() % 3 != 0) {
        return false;
    }
    std::vector<Vec3> pts;
    pts.reserve(it->size() / 3);
    for (usize i = 0; i < it->size(); i += 3) {
        const json& x = (*it)[i];
        const json& y = (*it)[i + 1];
        const json& z = (*it)[i + 2];
        if (!x.is_number() || !y.is_number() || !z.is_number()) {
            return false;
        }
        pts.emplace_back(x.get<f32>(), y.get<f32>(), z.get<f32>());
    }
    out = std::move(pts);
    return true;
}

bool indices_from(const json& j, std::vector<u32>& out) {
    const auto it = j.find("indices");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_array() || it->size() % 3 != 0) {
        return false;
    }
    std::vector<u32> idx;
    idx.reserve(it->size());
    for (const json& v : *it) {
        if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<i64>() >= 0)) {
            return false;
        }
        idx.push_back(v.get<u32>());
    }
    out = std::move(idx);
    return true;
}

std::string asset_to_string(const AssetId& id) { return id.is_valid() ? id.to_string() : std::string(); }

bool asset_from_string(const std::string& s, AssetId& out) {
    if (s.empty()) {
        out = AssetId{};
        return true;
    }
    if (s.size() != 32) {
        return false;
    }
    AssetId id;
    for (usize i = 0; i < 32; ++i) {
        const char c = s[i];
        u64        v = 0;
        if (c >= '0' && c <= '9') {
            v = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v = static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
        u64& half = i < 16 ? id.hi : id.lo;
        half      = (half << 4) | v;
    }
    out = id;
    return true;
}

} // namespace

bool register_gameplay_codecs(World& world) {
    bool ok = true;

    ok &= add_codec<RigidBodyComponent>(
        world, "RigidBody",
        [](const World&, const RigidBodyComponent& c) {
            return json{ { "motion", motion_name(c.motion_type) },
                         { "mass", c.mass },
                         { "friction", c.friction },
                         { "restitution", c.restitution },
                         { "linear_damping", c.linear_damping },
                         { "angular_damping", c.angular_damping },
                         { "gravity_factor", c.gravity_factor },
                         { "is_sensor", c.is_sensor },
                         { "continuous_collision", c.continuous_collision },
                         { "allow_sleeping", c.allow_sleeping },
                         { "collision_layer", c.collision_layer },
                         { "locked_dofs", c.locked_dofs },
                         { "initial_linear_velocity", to_json(c.initial_linear_velocity) },
                         { "initial_angular_velocity", to_json(c.initial_angular_velocity) } };
        },
        [](const World&, const json& j, RigidBodyComponent& c) {
            return motion_from(j, c.motion_type) && read(j, "mass", c.mass) && read(j, "friction", c.friction) &&
                   read(j, "restitution", c.restitution) && read(j, "linear_damping", c.linear_damping) &&
                   read(j, "angular_damping", c.angular_damping) && read(j, "gravity_factor", c.gravity_factor) &&
                   read(j, "is_sensor", c.is_sensor) && read(j, "continuous_collision", c.continuous_collision) &&
                   read(j, "allow_sleeping", c.allow_sleeping) && read(j, "collision_layer", c.collision_layer) &&
                   read(j, "locked_dofs", c.locked_dofs) &&
                   read(j, "initial_linear_velocity", c.initial_linear_velocity) &&
                   read(j, "initial_angular_velocity", c.initial_angular_velocity);
        });

    ok &= add_codec<ColliderComponent>(
        world, "Collider",
        [](const World&, const ColliderComponent& c) {
            json j{ { "shape", shape_name(c.shape) },
                    { "local_offset", to_json(c.local_offset) },
                    { "local_rotation", to_json(c.local_rotation) } };
            switch (c.shape) {
            case ColliderShape::Box: j["half_extents"] = to_json(c.half_extents); break;
            case ColliderShape::Sphere: j["radius"] = c.radius; break;
            case ColliderShape::Capsule:
            case ColliderShape::Cylinder:
                j["radius"]      = c.radius;
                j["half_height"] = c.half_height;
                break;
            case ColliderShape::ConvexHull: j["points"] = points_to_json(c.points); break;
            case ColliderShape::TriangleMesh:
                j["vertices"] = points_to_json(c.vertices);
                j["indices"]  = c.indices;
                break;
            }
            return j;
        },
        [](const World&, const json& j, ColliderComponent& c) {
            return shape_from(j, c.shape) && read(j, "local_offset", c.local_offset) &&
                   read(j, "local_rotation", c.local_rotation) && read(j, "half_extents", c.half_extents) &&
                   read(j, "radius", c.radius) && read(j, "half_height", c.half_height) &&
                   points_from(j, "points", c.points) && points_from(j, "vertices", c.vertices) &&
                   indices_from(j, c.indices);
        });

    ok &= add_codec<CharacterControllerComponent>(
        world, "CharacterController",
        [](const World&, const CharacterControllerComponent& c) {
            return json{ { "radius", c.radius },
                         { "half_height", c.half_height },
                         { "max_slope_deg", c.max_slope_deg },
                         { "step_up_height", c.step_up_height },
                         { "stick_to_floor_distance", c.stick_to_floor_distance },
                         { "mass", c.mass },
                         { "max_strength", c.max_strength },
                         { "collision_layer", c.collision_layer },
                         { "jump_speed", c.jump_speed },
                         { "apply_gravity", c.apply_gravity } };
        },
        [](const World&, const json& j, CharacterControllerComponent& c) {
            return read(j, "radius", c.radius) && read(j, "half_height", c.half_height) &&
                   read(j, "max_slope_deg", c.max_slope_deg) && read(j, "step_up_height", c.step_up_height) &&
                   read(j, "stick_to_floor_distance", c.stick_to_floor_distance) && read(j, "mass", c.mass) &&
                   read(j, "max_strength", c.max_strength) && read(j, "collision_layer", c.collision_layer) &&
                   read(j, "jump_speed", c.jump_speed) && read(j, "apply_gravity", c.apply_gravity);
        });

    ok &= add_codec<FixedConstraintComponent>(
        world, "FixedConstraint",
        [](const World& w, const FixedConstraintComponent& c) { return json{ { "target", entity_ref(w, c.target) } }; },
        [](const World& w, const json& j, FixedConstraintComponent& c) { return read_entity(w, j, "target", c.target); });

    ok &= add_codec<PointConstraintComponent>(
        world, "PointConstraint",
        [](const World& w, const PointConstraintComponent& c) {
            return json{ { "target", entity_ref(w, c.target) }, { "local_anchor", to_json(c.local_anchor) } };
        },
        [](const World& w, const json& j, PointConstraintComponent& c) {
            return read_entity(w, j, "target", c.target) && read(j, "local_anchor", c.local_anchor);
        });

    ok &= add_codec<HingeConstraintComponent>(
        world, "HingeConstraint",
        [](const World& w, const HingeConstraintComponent& c) {
            return json{ { "target", entity_ref(w, c.target) },
                         { "local_anchor", to_json(c.local_anchor) },
                         { "local_axis", to_json(c.local_axis) },
                         { "limits_enabled", c.limits_enabled },
                         { "min_angle_deg", c.min_angle_deg },
                         { "max_angle_deg", c.max_angle_deg },
                         { "max_friction_torque", c.max_friction_torque } };
        },
        [](const World& w, const json& j, HingeConstraintComponent& c) {
            return read_entity(w, j, "target", c.target) && read(j, "local_anchor", c.local_anchor) &&
                   read(j, "local_axis", c.local_axis) && read(j, "limits_enabled", c.limits_enabled) &&
                   read(j, "min_angle_deg", c.min_angle_deg) && read(j, "max_angle_deg", c.max_angle_deg) &&
                   read(j, "max_friction_torque", c.max_friction_torque);
        });

    ok &= add_codec<DistanceConstraintComponent>(
        world, "DistanceConstraint",
        [](const World& w, const DistanceConstraintComponent& c) {
            return json{ { "target", entity_ref(w, c.target) },
                         { "local_anchor", to_json(c.local_anchor) },
                         { "target_local_anchor", to_json(c.target_local_anchor) },
                         { "min_distance", c.min_distance },
                         { "max_distance", c.max_distance },
                         { "spring_frequency", c.spring_frequency },
                         { "spring_damping", c.spring_damping } };
        },
        [](const World& w, const json& j, DistanceConstraintComponent& c) {
            return read_entity(w, j, "target", c.target) && read(j, "local_anchor", c.local_anchor) &&
                   read(j, "target_local_anchor", c.target_local_anchor) &&
                   read(j, "min_distance", c.min_distance) && read(j, "max_distance", c.max_distance) &&
                   read(j, "spring_frequency", c.spring_frequency) && read(j, "spring_damping", c.spring_damping);
        });

    ok &= add_codec<FlyCameraComponent>(
        world, "FlyCamera",
        [](const World&, const FlyCameraComponent& c) {
            return json{ { "move_speed", c.move_speed },
                         { "boost_multiplier", c.boost_multiplier },
                         { "look_sensitivity", c.look_sensitivity },
                         { "speed_scroll_factor", c.speed_scroll_factor },
                         { "min_speed", c.min_speed },
                         { "max_speed", c.max_speed },
                         { "require_look_button", c.require_look_button } };
        },
        [](const World&, const json& j, FlyCameraComponent& c) {
            c.initialized = false; // re-derive yaw/pitch from the loaded transform
            return read(j, "move_speed", c.move_speed) && read(j, "boost_multiplier", c.boost_multiplier) &&
                   read(j, "look_sensitivity", c.look_sensitivity) &&
                   read(j, "speed_scroll_factor", c.speed_scroll_factor) && read(j, "min_speed", c.min_speed) &&
                   read(j, "max_speed", c.max_speed) && read(j, "require_look_button", c.require_look_button);
        });

    ok &= add_codec<OrbitCameraComponent>(
        world, "OrbitCamera",
        [](const World& w, const OrbitCameraComponent& c) {
            return json{ { "target", to_json(c.target) },
                         { "follow", entity_ref(w, c.follow) },
                         { "distance", c.distance },
                         { "min_distance", c.min_distance },
                         { "max_distance", c.max_distance },
                         { "yaw_deg", c.yaw_deg },
                         { "pitch_deg", c.pitch_deg },
                         { "orbit_sensitivity", c.orbit_sensitivity },
                         { "zoom_factor", c.zoom_factor },
                         { "pan_sensitivity", c.pan_sensitivity } };
        },
        [](const World& w, const json& j, OrbitCameraComponent& c) {
            return read(j, "target", c.target) && read_entity(w, j, "follow", c.follow) &&
                   read(j, "distance", c.distance) && read(j, "min_distance", c.min_distance) &&
                   read(j, "max_distance", c.max_distance) && read(j, "yaw_deg", c.yaw_deg) &&
                   read(j, "pitch_deg", c.pitch_deg) && read(j, "orbit_sensitivity", c.orbit_sensitivity) &&
                   read(j, "zoom_factor", c.zoom_factor) && read(j, "pan_sensitivity", c.pan_sensitivity);
        });

    ok &= add_codec<GIVolumeComponent>(
        world, "GIVolume",
        [](const World&, const GIVolumeComponent& c) {
            return json{ { "probe_spacing", c.probe_spacing }, { "intensity", c.intensity }, { "enabled", c.enabled } };
        },
        [](const World&, const json& j, GIVolumeComponent& c) {
            return read(j, "probe_spacing", c.probe_spacing) && read(j, "intensity", c.intensity) &&
                   read(j, "enabled", c.enabled);
        });

    ok &= add_codec<ReflectionProbeComponent>(
        world, "ReflectionProbe",
        [](const World&, const ReflectionProbeComponent& c) {
            return json{ { "intensity", c.intensity }, { "blend_distance", c.blend_distance }, { "enabled", c.enabled } };
        },
        [](const World&, const json& j, ReflectionProbeComponent& c) {
            return read(j, "intensity", c.intensity) && read(j, "blend_distance", c.blend_distance) &&
                   read(j, "enabled", c.enabled);
        });

    ok &= add_codec<MaterialOverridesComponent>(
        world, "MaterialOverrides",
        [](const World&, const MaterialOverridesComponent& c) {
            json list = json::array();
            for (const AssetId& id : c.materials) {
                list.push_back(asset_to_string(id));
            }
            return json{ { "materials", std::move(list) } };
        },
        [](const World&, const json& j, MaterialOverridesComponent& c) {
            const auto it = j.find("materials");
            if (it == j.end()) {
                return true;
            }
            if (!it->is_array()) {
                return false;
            }
            std::vector<AssetId> ids;
            for (const json& v : *it) {
                AssetId id;
                if (!v.is_string() || !asset_from_string(v.get<std::string>(), id)) {
                    return false;
                }
                ids.push_back(id);
            }
            c.materials = std::move(ids);
            return true;
        });

    if (!ok) {
        AE_LOG_ERROR("Gameplay", "failed to register some gameplay component codecs");
    }
    return ok;
}

bool register_default_codecs(World& world) {
    bool ok = register_gameplay_codecs(world);
#if AE_WITH_ANIMATION
    ok &= animation::register_animation_codecs(world);
#endif
#if AE_WITH_SCRIPTING
    ok &= scripting::register_script_component_codec(world);
#endif
    return ok;
}

} // namespace aether::gameplay
