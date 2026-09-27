// material_instance.cpp — see material_instance.h.
#include "aether/editor/material_instance.h"

#include "aether/gameplay/components.h"
#include "aether/scene/components.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

namespace aether::editor {

namespace {

using Json = nlohmann::json;

constexpr u64 kInstanceTag = 0xAE3D'4D41'5449'0000ull; // "AE3D" "MATI" in the hi word

u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

struct Fnv {
    u64  h = 0xcbf29ce484222325ull;
    void bytes(const void* p, usize n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (usize i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 0x100000001b3ull;
        }
    }
    template <class T>
    void pod(const T& v) {
        bytes(&v, sizeof(T));
    }
};

bool read_floats(const Json& j, const char* key, f32* out, int n) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != static_cast<usize>(n)) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (!(*it)[static_cast<usize>(i)].is_number()) {
            return false;
        }
        out[i] = (*it)[static_cast<usize>(i)].get<f32>();
    }
    return true;
}

void read_float(const Json& j, const char* key, f32& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        out = it->get<f32>();
    }
}

void read_id(const Json& j, const char* key, AssetId& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        AssetId id;
        if (parse_asset_id(it->get<std::string>(), id)) {
            out = id;
        }
    }
}

Json material_json(const assets::MaterialData& m) {
    Json j;
    j["name"]               = m.name;
    j["base_color"]         = Json::array({ m.base_color_factor.x, m.base_color_factor.y, m.base_color_factor.z, m.base_color_factor.w });
    j["emissive"]           = Json::array({ m.emissive_factor.x, m.emissive_factor.y, m.emissive_factor.z });
    j["metallic"]           = m.metallic_factor;
    j["roughness"]          = m.roughness_factor;
    j["normal_scale"]       = m.normal_scale;
    j["occlusion_strength"] = m.occlusion_strength;
    j["alpha_cutoff"]       = m.alpha_cutoff;
    j["alpha_mode"]         = m.alpha_mode == assets::AlphaMode::Blend ? "Blend" : m.alpha_mode == assets::AlphaMode::Mask ? "Mask" : "Opaque";
    j["double_sided"]       = m.double_sided;
    auto id                 = [](const AssetId& a) { return a.is_valid() ? a.to_string() : std::string(); };
    j["base_color_texture"]         = id(m.base_color_texture);
    j["metallic_roughness_texture"] = id(m.metallic_roughness_texture);
    j["normal_texture"]             = id(m.normal_texture);
    j["occlusion_texture"]          = id(m.occlusion_texture);
    j["emissive_texture"]           = id(m.emissive_texture);
    return j;
}

assets::MaterialData material_from(const Json& j) {
    assets::MaterialData m;
    if (const auto it = j.find("name"); it != j.end() && it->is_string()) {
        m.name = it->get<std::string>();
    }
    (void)read_floats(j, "base_color", &m.base_color_factor.x, 4);
    (void)read_floats(j, "emissive", &m.emissive_factor.x, 3);
    read_float(j, "metallic", m.metallic_factor);
    read_float(j, "roughness", m.roughness_factor);
    read_float(j, "normal_scale", m.normal_scale);
    read_float(j, "occlusion_strength", m.occlusion_strength);
    read_float(j, "alpha_cutoff", m.alpha_cutoff);
    if (const auto it = j.find("alpha_mode"); it != j.end() && it->is_string()) {
        const std::string mode = it->get<std::string>();
        m.alpha_mode           = mode == "Blend" ? assets::AlphaMode::Blend : mode == "Mask" ? assets::AlphaMode::Mask : assets::AlphaMode::Opaque;
    }
    if (const auto it = j.find("double_sided"); it != j.end() && it->is_boolean()) {
        m.double_sided = it->get<bool>();
    }
    read_id(j, "base_color_texture", m.base_color_texture);
    read_id(j, "metallic_roughness_texture", m.metallic_roughness_texture);
    read_id(j, "normal_texture", m.normal_texture);
    read_id(j, "occlusion_texture", m.occlusion_texture);
    read_id(j, "emissive_texture", m.emissive_texture);
    return m;
}

std::string dump(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

} // namespace

MaterialInstanceComponent::Slot* MaterialInstanceComponent::find(i32 slot) noexcept {
    for (Slot& s : slots) {
        if (s.slot == slot) {
            return &s;
        }
    }
    return nullptr;
}

const MaterialInstanceComponent::Slot* MaterialInstanceComponent::find(i32 slot) const noexcept {
    return const_cast<MaterialInstanceComponent*>(this)->find(slot);
}

AssetId material_instance_id(u64 entity_uuid, i32 slot) {
    AssetId id;
    id.hi = kInstanceTag | (static_cast<u64>(static_cast<u32>(slot)) & 0xFFFFull);
    id.lo = mix(entity_uuid ^ (static_cast<u64>(static_cast<u32>(slot)) << 48)) | 1ull;
    return id;
}

bool is_material_instance_id(const AssetId& id) { return (id.hi & ~0xFFFFull) == kInstanceTag; }

u64 material_data_hash(const assets::MaterialData& m) {
    Fnv f;
    f.bytes(m.name.data(), m.name.size());
    f.pod(m.base_color_factor);
    f.pod(m.emissive_factor);
    f.pod(m.metallic_factor);
    f.pod(m.roughness_factor);
    f.pod(m.normal_scale);
    f.pod(m.occlusion_strength);
    f.pod(m.alpha_cutoff);
    f.pod(m.alpha_mode);
    f.pod(m.double_sided);
    for (const AssetId* id : { &m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture,
                               &m.occlusion_texture, &m.emissive_texture }) {
        f.pod(id->hi);
        f.pod(id->lo);
    }
    return f.h;
}

bool parse_asset_id(StringView text, AssetId& out) {
    if (text.empty()) {
        out = AssetId{};
        return true;
    }
    if (text.size() != 32) {
        return false;
    }
    u64 words[2] = { 0, 0 };
    for (usize i = 0; i < 32; ++i) {
        const char c = text[i];
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
        words[i / 16] = (words[i / 16] << 4) | v;
    }
    out.hi = words[0];
    out.lo = words[1];
    return true;
}

std::string material_to_json(const assets::MaterialData& data) { return dump(material_json(data)); }

Result<assets::MaterialData> material_from_json(StringView json) {
    const Json j = Json::parse(json.begin(), json.end(), nullptr, false);
    if (!j.is_object()) {
        return make_error<assets::MaterialData>(ErrorCode::InvalidArgument, "material: expected a JSON object");
    }
    return material_from(j);
}

AssetId material_slot_target(const World& world, Entity e, i32 slot) {
    if (slot < 0) {
        const auto* mr = world.try_get<MeshRendererComponent>(e);
        return mr != nullptr ? mr->material : AssetId{};
    }
    const auto* mo = world.try_get<gameplay::MaterialOverridesComponent>(e);
    return mo != nullptr ? mo->slot(static_cast<u32>(slot)) : AssetId{};
}

void set_material_slot_target(World& world, Entity e, i32 slot, const AssetId& material) {
    if (slot < 0) {
        if (auto* mr = world.try_get<MeshRendererComponent>(e)) {
            mr->material = material;
        }
        return;
    }
    auto* mo = world.try_get<gameplay::MaterialOverridesComponent>(e);
    if (mo == nullptr) {
        mo = &world.add<gameplay::MaterialOverridesComponent>(e);
    }
    if (mo->materials.size() <= static_cast<usize>(slot)) {
        mo->materials.resize(static_cast<usize>(slot) + 1);
    }
    mo->materials[static_cast<usize>(slot)] = material;
}

usize bind_material_instances(World& world) {
    usize changed = 0;
    for (const auto [e, mi] : world.registry().view<const MaterialInstanceComponent>().each()) {
        const u64 uuid = scene::uuid_of(world, e);
        for (const auto& s : mi.slots) {
            if (s.slot < 0 && !world.has<MeshRendererComponent>(e)) {
                continue; // nothing to point at
            }
            const AssetId id = material_instance_id(uuid, s.slot);
            if (material_slot_target(world, e, s.slot) != id) {
                set_material_slot_target(world, e, s.slot, id);
                ++changed;
            }
        }
    }
    return changed;
}

void make_material_instance(World& world, Entity e, i32 slot, const assets::MaterialData& data) {
    if (!world.valid(e)) {
        return;
    }
    auto* mi = world.try_get<MaterialInstanceComponent>(e);
    if (mi == nullptr) {
        mi = &world.add<MaterialInstanceComponent>(e);
    }
    if (auto* s = mi->find(slot)) {
        s->data = data;
    } else {
        mi->slots.push_back({ slot, data });
        std::sort(mi->slots.begin(), mi->slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    }
    set_material_slot_target(world, e, slot, material_instance_id(scene::uuid_of(world, e), slot));
}

void remove_material_instance(World& world, Entity e, i32 slot, const AssetId& replacement) {
    auto* mi = world.valid(e) ? world.try_get<MaterialInstanceComponent>(e) : nullptr;
    if (mi == nullptr) {
        return;
    }
    std::erase_if(mi->slots, [&](const auto& s) { return s.slot == slot; });
    set_material_slot_target(world, e, slot, replacement);
    if (mi->slots.empty()) {
        world.registry().remove<MaterialInstanceComponent>(e);
    }
}

bool register_material_instance_codec(World& world) {
    scene::ComponentCodec codec;
    codec.name = std::string(kMaterialInstanceCodecName);
    codec.save = [](const World& w, Entity e, std::string& out) {
        const auto* mi = w.try_get<MaterialInstanceComponent>(e);
        if (mi == nullptr) {
            return false;
        }
        Json slots = Json::array();
        for (const auto& s : mi->slots) {
            Json j     = material_json(s.data);
            j["slot"]  = s.slot;
            slots.push_back(std::move(j));
        }
        out = dump(Json{ { "slots", std::move(slots) } });
        return true;
    };
    codec.load = [](World& w, Entity e, StringView text) -> Result<void> {
        const Json j  = Json::parse(text.begin(), text.end(), nullptr, false);
        const auto it = j.is_object() ? j.find("slots") : j.end();
        if (!j.is_object() || it == j.end() || !it->is_array()) {
            return make_error(ErrorCode::InvalidArgument, "EditorMaterialInstance: expected {\"slots\": [...]}");
        }
        MaterialInstanceComponent mi;
        for (const Json& s : *it) {
            if (!s.is_object()) {
                continue;
            }
            MaterialInstanceComponent::Slot slot;
            if (const auto si = s.find("slot"); si != s.end() && si->is_number_integer()) {
                slot.slot = si->get<i32>();
            }
            slot.data = material_from(s);
            mi.slots.push_back(std::move(slot));
        }
        w.add<MaterialInstanceComponent>(e, std::move(mi));
        return {};
    };
    return scene::register_component_codec(world, std::move(codec));
}

} // namespace aether::editor
