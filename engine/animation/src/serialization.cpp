// serialization.cpp — "Animator" ComponentCodec (JSON via nlohmann, private to this TU).
#include "aether/animation/serialization.h"

#include "aether/animation/components.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <nlohmann/json.hpp>

#include <string>

namespace aether::animation {

namespace {

using json = nlohmann::json;

std::string to_hex(const AssetId& id) { return id.is_valid() ? id.to_string() : std::string{}; }

bool from_hex(const std::string& s, AssetId& out) {
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
        half = (half << 4) | v;
    }
    out = id;
    return true;
}

const char* mode_name(RootMotionMode m) {
    switch (m) {
    case RootMotionMode::Extract: return "extract";
    case RootMotionMode::ApplyToTransform: return "apply";
    case RootMotionMode::Ignore: break;
    }
    return "ignore";
}

bool save_animator(const World& world, Entity e, std::string& out) {
    const auto* a = world.try_get<AnimatorComponent>(e);
    if (a == nullptr) {
        return false;
    }
    const RootMotionSettings& rm = a->root_motion_settings();
    json                      root_motion = {{"mode", mode_name(a->root_motion_mode())},
                                             {"horizontal", rm.extract_horizontal},
                                             {"vertical", rm.extract_vertical},
                                             {"yaw", rm.extract_yaw}};
    if (rm.joint != kInvalidJoint) {
        root_motion["joint_index"] = rm.joint;
        if (a->skeleton() && rm.joint < a->skeleton()->joint_count()) {
            root_motion["joint"] = a->skeleton()->joint_name(rm.joint);
        }
    }
    const json j = {{"skeleton", to_hex(a->skeleton_asset)},
                    {"graph", to_hex(a->graph_asset)},
                    {"playback_rate", a->playback_rate()},
                    {"paused", a->paused()},
                    {"enabled", a->enabled()},
                    {"root_motion", std::move(root_motion)}};
    out = j.dump();
    return true;
}

Result<void> load_animator(World& world, Entity e, StringView text) {
    const json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        return Error{ErrorCode::InvalidArgument, "Animator: expected a JSON object"};
    }
    AssetId skeleton_id, graph_id;
    if (!from_hex(j.value("skeleton", std::string{}), skeleton_id) ||
        !from_hex(j.value("graph", std::string{}), graph_id)) {
        return Error{ErrorCode::InvalidArgument, "Animator: malformed asset id"};
    }
    // Keep runtime bindings when the component already exists (e.g. reloading settings).
    AnimatorComponent* a = world.try_get<AnimatorComponent>(e);
    if (a == nullptr) {
        a = &world.add<AnimatorComponent>(e);
    }
    a->skeleton_asset = skeleton_id;
    a->graph_asset = graph_id;
    a->set_playback_rate(j.value("playback_rate", 1.0f));
    a->set_paused(j.value("paused", false));
    a->set_enabled(j.value("enabled", true));

    RootMotionMode     mode = RootMotionMode::Ignore;
    RootMotionSettings rm;
    if (const auto it = j.find("root_motion"); it != j.end() && it->is_object()) {
        const std::string m = it->value("mode", std::string("ignore"));
        mode = m == "extract" ? RootMotionMode::Extract
               : m == "apply" ? RootMotionMode::ApplyToTransform
                              : RootMotionMode::Ignore;
        rm.extract_horizontal = it->value("horizontal", true);
        rm.extract_vertical = it->value("vertical", false);
        rm.extract_yaw = it->value("yaw", true);
        rm.joint = it->value("joint_index", kInvalidJoint);
        const std::string name = it->value("joint", std::string{});
        if (!name.empty() && a->skeleton()) {
            if (const u32 found = a->skeleton()->find_joint(name); found != kInvalidJoint) {
                rm.joint = found;
            }
        }
    }
    a->set_root_motion(mode, rm);
    return {};
}

} // namespace

bool register_animation_codecs(World& world) {
    scene::ComponentCodec codec;
    codec.name = kAnimatorCodecName;
    codec.save = &save_animator;
    codec.load = &load_animator;
    return scene::register_component_codec(world, std::move(codec));
}

} // namespace aether::animation
