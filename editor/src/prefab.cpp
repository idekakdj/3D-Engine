// prefab.cpp — see prefab.h. JSON is handled with nlohmann (non-throwing parse, typed checks).
#include "aether/editor/prefab.h"

#include "aether/editor/material_instance.h"
#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace aether::editor {

namespace {

using Json = nlohmann::json;

Json vec_json(const f32* v, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) {
        a.push_back(v[i]);
    }
    return a;
}

void write_transform(Json& components, const Transform& t) {
    Json& tr        = components["Transform"];
    tr["position"]  = vec_json(&t.position.x, 3);
    tr["rotation"]  = Json::array({ t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w });
    tr["scale"]     = vec_json(&t.scale.x, 3);
}

} // namespace

// ---- codecs -------------------------------------------------------------------------------------
bool register_editor_codecs(World& world) {
    scene::ComponentCodec prefab;
    prefab.name = std::string(kPrefabCodecName);
    prefab.save = [](const World& w, Entity e, std::string& out) {
        const auto* c = w.try_get<PrefabInstanceComponent>(e);
        if (c == nullptr) {
            return false;
        }
        out = Json{ { "source", c->source }, { "root", c->root_index } }.dump(-1, ' ', false, Json::error_handler_t::replace);
        return true;
    };
    prefab.load = [](World& w, Entity e, StringView text) -> Result<void> {
        const Json j = Json::parse(text.begin(), text.end(), nullptr, false);
        const auto it = j.is_object() ? j.find("source") : j.end();
        if (!j.is_object() || it == j.end() || !it->is_string()) {
            return make_error(ErrorCode::InvalidArgument, "EditorPrefabInstance: expected {\"source\": string}");
        }
        PrefabInstanceComponent c{ it->get<std::string>(), 0 };
        if (const auto root = j.find("root"); root != j.end() && root->is_number_unsigned()) {
            c.root_index = root->get<u32>();
        }
        w.add<PrefabInstanceComponent>(e, std::move(c));
        return {};
    };
    const bool ok = scene::register_component_codec(world, std::move(prefab));
    return register_material_instance_codec(world) && ok;
}

// ---- files --------------------------------------------------------------------------------------
Result<std::string> read_text_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return make_error<std::string>(ErrorCode::NotFound, "cannot open " + file.generic_string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

namespace {
Result<void> write_text_file(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::IoError, "cannot write " + tmp.generic_string());
        }
        out << text;
        if (!out) {
            return make_error(ErrorCode::IoError, "write failed: " + tmp.generic_string());
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return make_error(ErrorCode::IoError, "cannot replace " + file.generic_string());
    }
    return {};
}
} // namespace

// ---- documents ----------------------------------------------------------------------------------
Result<std::string> make_prefab_document(const World& world, std::span<const Entity> roots, const Mat4& pivot) {
    auto text = scene::save_entities_to_string(world, roots);
    if (!text) {
        return text.error();
    }
    Json doc = Json::parse(*text, nullptr, false);
    if (!doc.is_object() || !doc.contains("entities") || !doc["entities"].is_array()) {
        return make_error<std::string>(ErrorCode::Internal, "prefab: unexpected serializer output");
    }
    const Mat4 inv_pivot = glm::inverse(pivot);
    for (Json& entry : doc["entities"]) {
        if (!entry.is_object() || entry.contains("parent")) {
            continue; // only top-level entities are re-based
        }
        u64 uuid = 0;
        if (entry.contains("uuid") && entry["uuid"].is_string()) {
            (void)scene::uuid_from_string(entry["uuid"].get<std::string>(), uuid);
        }
        const Entity e = uuid != 0 ? scene::find_by_uuid(world, uuid) : kNullEntity;
        if (e == kNullEntity || !entry.contains("components") || !entry["components"].is_object()) {
            continue;
        }
        Json& components = entry["components"];
        components.erase(std::string(kPrefabCodecName)); // the prefab does not link to itself
        write_transform(components, scene::decompose_transform(inv_pivot * world.world_matrix(e)));
    }
    return doc.dump(2, ' ', false, Json::error_handler_t::replace);
}

Result<void> write_prefab(const World& world, std::span<const Entity> roots, const Mat4& pivot,
                          const std::filesystem::path& file) {
    auto doc = make_prefab_document(world, roots, pivot);
    if (!doc) {
        return doc.error();
    }
    return write_text_file(file, *doc);
}

Result<std::vector<Entity>> instantiate_prefab_document(World& world, StringView json, StringView source, Entity parent,
                                                        const Mat4& placement) {
    auto roots = scene::load_entities_from_string(world, json, parent);
    if (!roots) {
        return roots.error();
    }
    for (usize i = 0; i < roots->size(); ++i) {
        const Entity    e     = (*roots)[i];
        const Transform local = scene::local_transform(world, e);
        scene::set_local_transform(world, e, scene::decompose_transform(placement * scene::compose_transform(local)));
        if (!source.empty()) {
            world.add<PrefabInstanceComponent>(e, PrefabInstanceComponent{ std::string(source), static_cast<u32>(i) });
        } else {
            world.registry().remove<PrefabInstanceComponent>(e);
        }
    }
    world.update_transforms();
    return roots;
}

Result<std::vector<Entity>> instantiate_prefab(World& world, const std::filesystem::path& file, StringView source,
                                               Entity parent, const Mat4& placement) {
    auto text = read_text_file(file);
    if (!text) {
        return text.error();
    }
    return instantiate_prefab_document(world, *text, source, parent, placement);
}

usize prefab_root_count(StringView json) {
    const Json doc = Json::parse(json.begin(), json.end(), nullptr, false);
    if (!doc.is_object() || !doc.contains("entities") || !doc["entities"].is_array()) {
        return 0;
    }
    usize n = 0;
    for (const Json& entry : doc["entities"]) {
        n += entry.is_object() && !entry.contains("parent") ? 1u : 0u;
    }
    return n;
}

Result<void> apply_prefab_instance(const World& world, Entity instance_root, const std::filesystem::path& file) {
    if (!world.valid(instance_root)) {
        return make_error(ErrorCode::InvalidArgument, "apply prefab: invalid entity");
    }
    if (auto existing = read_text_file(file); existing && prefab_root_count(*existing) > 1) {
        return make_error(ErrorCode::Unsupported, "apply prefab: multi-root prefabs cannot be applied from one instance");
    }
    const Entity roots[] = { instance_root };
    return write_prefab(world, roots, world.world_matrix(instance_root), file);
}

Result<Entity> revert_prefab_instance(World& world, Entity instance_root, const std::filesystem::path& file) {
    if (!world.valid(instance_root)) {
        return make_error<Entity>(ErrorCode::InvalidArgument, "revert prefab: invalid entity");
    }
    const auto*       link   = world.try_get<PrefabInstanceComponent>(instance_root);
    const std::string source = link != nullptr ? link->source : std::string();
    const usize       index  = link != nullptr ? link->root_index : 0u;
    auto text = read_text_file(file);
    if (!text) {
        return text.error();
    }
    const Entity parent = scene::parent_of(world, instance_root);
    auto         fresh  = instantiate_prefab_document(world, *text, source, parent, Mat4(1.0f));
    if (!fresh) {
        return fresh.error();
    }
    if (fresh->empty()) {
        return make_error<Entity>(ErrorCode::InvalidArgument, "revert prefab: empty prefab");
    }
    if (index >= fresh->size()) {
        for (const Entity x : *fresh) {
            world.destroy(x);
        }
        return make_error<Entity>(ErrorCode::NotFound, "revert prefab: the prefab no longer has that root");
    }
    // Only this instance's top-level entity replaces it; the prefab's other roots are discarded.
    for (usize i = 0; i < fresh->size(); ++i) {
        if (i != index) {
            world.destroy((*fresh)[i]);
        }
    }
    const Entity e = (*fresh)[index];
    scene::set_local_transform(world, e, scene::local_transform(world, instance_root));
    world.get<NameComponent>(e).name = world.get<NameComponent>(instance_root).name;
    (void)scene::move_before(world, e, instance_root);
    world.destroy(instance_root);
    world.update_transforms();
    return e;
}

} // namespace aether::editor
