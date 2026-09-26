// scene_serializer.cpp — .aescene JSON save/load and subtree copy/paste (format documented in
// aether/scene/scene_serializer.h). nlohmann/json is a private dependency of this TU.
//
// Error policy: the only exception source (JSON parsing) is caught at the boundary and
// converted to an Error; every other JSON access is type-checked first and serialization uses
// the replacing UTF-8 handler, so nothing throws out of this file.
#include "aether/core/handle.h"
#include "aether/core/log.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/visibility.h"
#include "scene_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace aether::scene {
namespace {

using Json = nlohmann::ordered_json; // keeps authoring order: diff-friendly, human-readable

constexpr const char* kCat = detail::kLogCategory;

// =================================================================================================
// Encoding helpers
// =================================================================================================

// A double whose shortest decimal form reads back as exactly `f` (so files show 0.1, not
// 0.10000000149011612). Non-finite values are written as JSON null.
double encode_float(f32 f) {
    if (!std::isfinite(f)) {
        return static_cast<double>(f);
    }
    char       buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), f);
    if (res.ec == std::errc{}) {
        double     d    = 0.0;
        const auto back = std::from_chars(buf, res.ptr, d);
        if (back.ec == std::errc{} && static_cast<f32>(d) == f &&
            std::signbit(d) == std::signbit(f)) {
            return d;
        }
    }
    return static_cast<double>(f); // exact fallback
}

Json vec3_json(const Vec3& v) {
    return Json::array({ encode_float(v.x), encode_float(v.y), encode_float(v.z) });
}

Json quat_json(const Quat& q) { // [x, y, z, w] (glTF order)
    return Json::array(
        { encode_float(q.x), encode_float(q.y), encode_float(q.z), encode_float(q.w) });
}

String asset_to_hex(const AssetId& id) {
    return uuid_to_string(id.hi) + uuid_to_string(id.lo);
}

bool asset_from_hex(StringView s, AssetId& out) {
    if (s.size() != 32) {
        return false;
    }
    AssetId id;
    if (!uuid_from_string(s.substr(0, 16), id.hi) || !uuid_from_string(s.substr(16), id.lo)) {
        return false;
    }
    out = id;
    return true;
}

const char* light_kind_name(LightKind kind) {
    switch (kind) {
    case LightKind::Directional: return "Directional";
    case LightKind::Point: return "Point";
    case LightKind::Spot: return "Spot";
    }
    return "Point";
}

std::string display(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string(); // never throws on unrepresentable characters
    return std::string(u8.begin(), u8.end());
}

constexpr const char* kBuiltinComponents[] = { "Name",         "Transform", "Visibility", "Tag",
                                               "MeshRenderer", "Light",     "Camera" };

bool is_builtin(StringView name) {
    for (const char* b : kBuiltinComponents) {
        if (name == b) {
            return true;
        }
    }
    return false;
}

const std::vector<ComponentCodec>& codecs_of(const World& world) {
    return detail::context(world.registry()).codecs;
}

// =================================================================================================
// Writing
// =================================================================================================

Json components_json(const World& world, Entity e) {
    Json c = Json::object();
    if (const auto* n = world.try_get<NameComponent>(e)) {
        Json o    = Json::object();
        o["name"] = n->name;
        c["Name"] = std::move(o);
    }
    if (const auto* t = world.try_get<TransformComponent>(e)) {
        Json o        = Json::object();
        o["position"] = vec3_json(t->local.position);
        o["rotation"] = quat_json(t->local.rotation);
        o["scale"]    = vec3_json(t->local.scale);
        c["Transform"] = std::move(o);
    }
    if (const auto* v = world.try_get<VisibilityComponent>(e)) {
        Json o          = Json::object();
        o["visible"]    = v->visible;
        c["Visibility"] = std::move(o);
    }
    if (const auto* t = world.try_get<TagComponent>(e)) {
        Json o   = Json::object();
        o["tag"] = t->tag;
        c["Tag"] = std::move(o);
    }
    if (const auto* m = world.try_get<MeshRendererComponent>(e)) {
        Json bounds   = Json::object();
        bounds["min"] = vec3_json(m->local_bounds.min);
        bounds["max"] = vec3_json(m->local_bounds.max);
        Json o            = Json::object();
        o["mesh"]         = asset_to_hex(m->mesh);
        o["material"]     = asset_to_hex(m->material);
        o["cast_shadows"] = m->cast_shadows;
        o["local_bounds"] = std::move(bounds);
        c["MeshRenderer"] = std::move(o);
    }
    if (const auto* l = world.try_get<LightComponent>(e)) {
        Json o              = Json::object();
        o["kind"]           = light_kind_name(l->kind);
        o["color"]          = vec3_json(l->color);
        o["intensity"]      = encode_float(l->intensity);
        o["range"]          = encode_float(l->range);
        o["inner_cone_deg"] = encode_float(l->inner_cone_deg);
        o["outer_cone_deg"] = encode_float(l->outer_cone_deg);
        o["cast_shadows"]   = l->cast_shadows;
        c["Light"]          = std::move(o);
    }
    if (const auto* cam = world.try_get<CameraComponent>(e)) {
        Json o         = Json::object();
        o["fov_y_deg"] = encode_float(cam->fov_y_deg);
        o["near_z"]    = encode_float(cam->near_z);
        o["far_z"]     = encode_float(cam->far_z);
        o["primary"]   = cam->primary;
        c["Camera"]    = std::move(o);
    }
    for (const ComponentCodec& codec : codecs_of(world)) {
        std::string text;
        if (!codec.save(world, e, text)) {
            continue;
        }
        Json value = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
        if (value.is_discarded()) {
            AE_LOG_ERROR(kCat, "scene save: codec '{}' produced invalid JSON for entity {}; "
                               "component omitted", codec.name, detail::to_string(e));
            continue;
        }
        c[codec.name] = std::move(value);
    }
    return c;
}

// Serializes `order` (pre-order: parents before children). A parent reference is written only
// when the parent is itself part of the document.
std::string write_document(const World& world, const std::vector<Entity>& order, const char* kind) {
    // Stable uuids for the file: the entity's IdComponent, or a synthesized one for entities
    // created through the raw registry without an identity.
    std::unordered_map<Entity, u64> uuid_for;
    std::unordered_set<u64>         used;
    uuid_for.reserve(order.size());
    used.reserve(order.size());
    u64  synth_state = 0x5CE4E5B9A1B2C3D4ull;
    auto synthesize  = [&]() {
        for (;;) {
            synth_state += 0x9E3779B97F4A7C15ull;
            u64 z = synth_state;
            z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z     = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            if (z != kInvalidUuid && !used.contains(z) && find_by_uuid(world, z) == kNullEntity) {
                return z;
            }
        }
    };
    for (const Entity e : order) {
        u64 uuid = uuid_of(world, e);
        if (uuid == kInvalidUuid || used.contains(uuid)) {
            uuid = synthesize();
        }
        used.insert(uuid);
        uuid_for.emplace(e, uuid);
    }

    Json entities = Json::array();
    for (const Entity e : order) {
        Json je    = Json::object();
        je["uuid"] = uuid_to_string(uuid_for.at(e));
        const Entity parent = parent_of(world, e);
        if (const auto it = uuid_for.find(parent); parent != kNullEntity && it != uuid_for.end()) {
            je["parent"] = uuid_to_string(it->second);
        }
        je["components"] = components_json(world, e);
        entities.push_back(std::move(je));
    }

    Json doc        = Json::object();
    doc["format"]   = std::string(kSceneFormatName);
    doc["version"]  = kSceneFormatVersion;
    doc["kind"]     = kind;
    doc["entities"] = std::move(entities);
    std::string text = doc.dump(2, ' ', false, Json::error_handler_t::replace);
    text.push_back('\n');
    return text;
}

// =================================================================================================
// Reading
// =================================================================================================

bool read_f32(const Json& j, f32& out) {
    if (!j.is_number()) {
        return false;
    }
    out = static_cast<f32>(j.get<double>());
    return true;
}

bool read_vec3(const Json& j, Vec3& out) {
    if (!j.is_array() || j.size() != 3) {
        return false;
    }
    Vec3 v;
    if (!read_f32(j[0], v.x) || !read_f32(j[1], v.y) || !read_f32(j[2], v.z)) {
        return false;
    }
    out = v;
    return true;
}

bool read_quat(const Json& j, Quat& out) {
    if (!j.is_array() || j.size() != 4) {
        return false;
    }
    f32 x = 0, y = 0, z = 0, w = 0;
    if (!read_f32(j[0], x) || !read_f32(j[1], y) || !read_f32(j[2], z) || !read_f32(j[3], w)) {
        return false;
    }
    if (x == 0.0f && y == 0.0f && z == 0.0f && w == 0.0f) {
        return false; // not a rotation
    }
    out = Quat(w, x, y, z);
    return true;
}

bool read_bool(const Json& j, bool& out) {
    if (!j.is_boolean()) {
        return false;
    }
    out = j.get<bool>();
    return true;
}

bool read_u32(const Json& j, u32& out) {
    if (!j.is_number_unsigned()) {
        return false; // non-negative integers parse as unsigned
    }
    const u64 v = j.get<u64>();
    if (v > std::numeric_limits<u32>::max()) {
        return false;
    }
    out = static_cast<u32>(v);
    return true;
}

bool read_string(const Json& j, std::string& out) {
    if (!j.is_string()) {
        return false;
    }
    out = j.get_ref<const std::string&>();
    return true;
}

bool read_asset(const Json& j, AssetId& out) {
    return j.is_string() && asset_from_hex(j.get_ref<const std::string&>(), out);
}

bool read_light_kind(const Json& j, LightKind& out) {
    if (!j.is_string()) {
        return false;
    }
    const std::string& s = j.get_ref<const std::string&>();
    if (s == "Directional") {
        out = LightKind::Directional;
    } else if (s == "Point") {
        out = LightKind::Point;
    } else if (s == "Spot") {
        out = LightKind::Spot;
    } else {
        return false;
    }
    return true;
}

// Per-document load state + warning accounting.
struct Loader {
    struct Deferred {
        Entity      e;
        usize       codec; // index into the world's codecs
        const Json* value;
        usize       entry;
    };

    World&                world;
    usize                 warnings = 0;
    usize                 entry    = 0; // 1-based index of the entity entry being processed
    std::vector<Deferred> deferred;     // codec components, applied once the hierarchy exists

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        ++warnings;
        AE_LOG_WARN(kCat, "scene load: entity #{}: {}", entry,
                    std::format(fmt, std::forward<Args>(args)...));
    }

    // Reads obj[key] into `out` if present; warns (keeping the old value) if malformed.
    template <typename T, typename ReadFn>
    void field(const Json& obj, const char* component, const char* key, T& out, ReadFn read) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            return;
        }
        T value = out;
        if (!read(*it, value)) {
            warn("{}.{} has an invalid value; default kept", component, key);
            return;
        }
        out = value;
    }

    static constexpr usize kNoCodec = static_cast<usize>(-1);

    [[nodiscard]] usize find_codec(StringView name) const {
        const std::vector<ComponentCodec>& codecs = codecs_of(world);
        for (usize i = 0; i < codecs.size(); ++i) {
            if (codecs[i].name == name) {
                return i;
            }
        }
        return kNoCodec;
    }

    void apply_deferred() {
        for (const Deferred& d : deferred) {
            entry = d.entry;
            if (!world.valid(d.e) || d.codec >= codecs_of(world).size()) {
                continue;
            }
            // By value: a load callback may (un)register codecs and reallocate the list.
            const ComponentCodec codec = codecs_of(world)[d.codec];
            const std::string    text  = d.value->dump(-1, ' ', false, Json::error_handler_t::replace);
            if (auto r = codec.load(world, d.e, text); !r) {
                warn("component '{}' failed to load ({}); skipped", codec.name, r.error().message);
            }
        }
    }

    void apply_components(Entity e, const Json& comps) {
        for (auto it = comps.begin(); it != comps.end(); ++it) {
            const std::string& key = it.key();
            const Json&        c   = it.value();
            if (!is_builtin(key)) {
                if (const usize codec = find_codec(key); codec != kNoCodec) {
                    deferred.push_back({ e, codec, &c, entry });
                } else {
                    warn("unknown component '{}' skipped", key);
                }
                continue;
            }
            if (!c.is_object()) {
                warn("component '{}' is not an object; skipped", key);
                continue;
            }
            if (key == "Name") {
                field(c, "Name", "name", world.get<NameComponent>(e).name, read_string);
            } else if (key == "Transform") {
                TransformComponent& t = world.get<TransformComponent>(e);
                field(c, "Transform", "position", t.local.position, read_vec3);
                field(c, "Transform", "rotation", t.local.rotation, read_quat);
                field(c, "Transform", "scale", t.local.scale, read_vec3);
                t.dirty = true;
            } else if (key == "Visibility") {
                bool visible = true;
                field(c, "Visibility", "visible", visible, read_bool);
                set_visible(world, e, visible);
            } else if (key == "Tag") {
                TagComponent tag;
                field(c, "Tag", "tag", tag.tag, read_u32);
                world.add<TagComponent>(e, tag);
            } else if (key == "MeshRenderer") {
                MeshRendererComponent m;
                field(c, "MeshRenderer", "mesh", m.mesh, read_asset);
                field(c, "MeshRenderer", "material", m.material, read_asset);
                field(c, "MeshRenderer", "cast_shadows", m.cast_shadows, read_bool);
                if (const auto b = c.find("local_bounds"); b != c.end()) {
                    if (b->is_object()) {
                        field(*b, "MeshRenderer.local_bounds", "min", m.local_bounds.min, read_vec3);
                        field(*b, "MeshRenderer.local_bounds", "max", m.local_bounds.max, read_vec3);
                    } else {
                        warn("MeshRenderer.local_bounds is not an object; default kept");
                    }
                }
                world.add<MeshRendererComponent>(e, m);
            } else if (key == "Light") {
                LightComponent l;
                field(c, "Light", "kind", l.kind, read_light_kind);
                field(c, "Light", "color", l.color, read_vec3);
                field(c, "Light", "intensity", l.intensity, read_f32);
                field(c, "Light", "range", l.range, read_f32);
                field(c, "Light", "inner_cone_deg", l.inner_cone_deg, read_f32);
                field(c, "Light", "outer_cone_deg", l.outer_cone_deg, read_f32);
                field(c, "Light", "cast_shadows", l.cast_shadows, read_bool);
                world.add<LightComponent>(e, l);
            } else if (key == "Camera") {
                CameraComponent cam;
                field(c, "Camera", "fov_y_deg", cam.fov_y_deg, read_f32);
                field(c, "Camera", "near_z", cam.near_z, read_f32);
                field(c, "Camera", "far_z", cam.far_z, read_f32);
                field(c, "Camera", "primary", cam.primary, read_bool);
                world.add<CameraComponent>(e, cam);
            }
        }
    }
};

// Parses and validates the envelope. On success `out` holds the document and `entities`
// points at its entity array.
Result<void> parse_document(StringView text, Json& out, const Json*& entities) {
    try {
        out = Json::parse(text.begin(), text.end());
    } catch (const Json::exception& ex) {
        return make_error(ErrorCode::InvalidArgument, std::format("scene: invalid JSON ({})", ex.what()));
    }
    if (!out.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "scene: document root must be an object");
    }
    const auto format = out.find("format");
    if (format == out.end() || !format->is_string() ||
        format->get_ref<const std::string&>() != kSceneFormatName) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("scene: missing or wrong \"format\" (expected \"{}\")",
                                      kSceneFormatName));
    }
    const auto version = out.find("version");
    if (version == out.end() || !version->is_number_integer()) {
        return make_error(ErrorCode::InvalidArgument, "scene: missing or non-integer \"version\"");
    }
    const i64 v = version->get<i64>();
    if (v < 1 || v > static_cast<i64>(kSceneFormatVersion)) {
        return make_error(ErrorCode::Unsupported,
                          std::format("scene: version {} is not supported (this build reads 1..{})",
                                      v, kSceneFormatVersion));
    }
    const auto ents = out.find("entities");
    if (ents == out.end() || !ents->is_array()) {
        return make_error(ErrorCode::InvalidArgument, "scene: missing \"entities\" array");
    }
    entities = &*ents;
    return {};
}

// Creates every entity of the document, then links the hierarchy in document order (so
// sibling order = document order, whatever order parents and children appear in).
SceneLoadResult instantiate(World& world, const Json& entities, bool keep_uuids, Entity attach) {
    struct Pending {
        Entity e;
        u64    parent_uuid;
        bool   has_parent;
        usize  entry;
    };
    Loader                          loader{ world };
    std::vector<Pending>            pending;
    std::unordered_map<u64, Entity> by_file_uuid;
    pending.reserve(entities.size());
    by_file_uuid.reserve(entities.size());

    for (const Json& je : entities) {
        ++loader.entry;
        if (!je.is_object()) {
            loader.warn("entry is not an object; skipped");
            continue;
        }
        u64 file_uuid = kInvalidUuid;
        if (const auto it = je.find("uuid"); it != je.end()) {
            if (!it->is_string() ||
                !uuid_from_string(it->get_ref<const std::string&>(), file_uuid) ||
                file_uuid == kInvalidUuid) {
                loader.warn("invalid \"uuid\"; a fresh one is assigned");
                file_uuid = kInvalidUuid;
            }
        }
        u64  parent_uuid = kInvalidUuid;
        bool has_parent  = false;
        if (const auto it = je.find("parent"); it != je.end() && !it->is_null()) {
            has_parent = it->is_string() &&
                         uuid_from_string(it->get_ref<const std::string&>(), parent_uuid) &&
                         parent_uuid != kInvalidUuid;
            if (!has_parent) {
                loader.warn("invalid \"parent\" reference; loaded as top-level");
            }
        }
        const Json* comps = nullptr;
        if (const auto it = je.find("components"); it != je.end()) {
            if (it->is_object()) {
                comps = &*it;
            } else {
                loader.warn("\"components\" is not an object; ignored");
            }
        }

        const u64    requested = keep_uuids ? file_uuid : kInvalidUuid;
        const Entity e         = detail::create_entity(world, "Entity", kNullEntity, requested);
        if (requested != kInvalidUuid && uuid_of(world, e) != requested) {
            ++loader.warnings; // uuid collision; the Id listener logged and re-keyed it
        }
        if (comps != nullptr) {
            loader.apply_components(e, *comps);
        }
        if (file_uuid != kInvalidUuid && !by_file_uuid.emplace(file_uuid, e).second) {
            loader.warn("duplicate uuid {} in document; references resolve to the first",
                        uuid_to_string(file_uuid));
        }
        pending.push_back({ e, parent_uuid, has_parent, loader.entry });
    }

    SceneLoadResult result;
    result.entity_count = pending.size();
    for (const Pending& p : pending) {
        loader.entry = p.entry;
        if (p.has_parent) {
            const auto it = by_file_uuid.find(p.parent_uuid);
            if (it == by_file_uuid.end()) {
                loader.warn("parent {} not found in document; loaded as top-level",
                            uuid_to_string(p.parent_uuid));
            } else if (detail::reparent(world, p.e, it->second, kNullEntity, true)) {
                continue;
            } else {
                loader.warn("parent {} rejected (self/cycle); loaded as top-level",
                            uuid_to_string(p.parent_uuid));
            }
        }
        if (attach != kNullEntity && !detail::reparent(world, p.e, attach, kNullEntity, true)) {
            loader.warn("could not attach under {}; left as a root", detail::to_string(attach));
        }
        result.roots.push_back(p.e);
    }
    loader.apply_deferred();
    result.warning_count = loader.warnings;

    world.update_transforms(); // world matrices valid as soon as the load returns
    return result;
}

Result<std::string> read_text(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return make_error<std::string>(ErrorCode::NotFound,
                                       std::format("scene: file not found '{}'", display(file)));
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return make_error<std::string>(ErrorCode::IoError,
                                       std::format("scene: cannot open '{}'", display(file)));
    }
    std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    if (in.bad()) {
        return make_error<std::string>(ErrorCode::IoError,
                                       std::format("scene: read error on '{}'", display(file)));
    }
    return text;
}

// Temp file + rename, so a crash mid-save never leaves a truncated scene behind.
Result<void> write_text_atomic(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec); // open() reports failures
    }
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::IoError,
                              std::format("scene: cannot write '{}'", display(tmp)));
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return make_error(ErrorCode::IoError,
                              std::format("scene: write error on '{}'", display(tmp)));
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return make_error(ErrorCode::IoError, std::format("scene: cannot replace '{}' ({})",
                                                          display(file), ec.message()));
    }
    return {};
}

std::vector<Entity> scene_order(const World& world) {
    std::vector<Entity> order;
    order.reserve(world.entity_count());
    for_each_in_hierarchy(world, [&](Entity e) { order.push_back(e); });
    // Entities outside the hierarchy (raw registry entities) are saved as roots.
    for (auto [e] : world.registry().storage<Entity>()->each()) {
        if (!world.has<HierarchyComponent>(e)) {
            order.push_back(e);
        }
    }
    return order;
}

} // namespace

// =================================================================================================
// Public API
// =================================================================================================

bool register_component_codec(World& world, ComponentCodec codec) {
    if (codec.name.empty() || !codec.save || !codec.load) {
        AE_LOG_ERROR(kCat, "register_component_codec: codec '{}' needs a name, save and load",
                     codec.name);
        return false;
    }
    if (is_builtin(codec.name)) {
        AE_LOG_ERROR(kCat, "register_component_codec: '{}' is a built-in component name",
                     codec.name);
        return false;
    }
    std::vector<ComponentCodec>& codecs = detail::context(world.registry()).codecs;
    for (ComponentCodec& existing : codecs) {
        if (existing.name == codec.name) {
            existing = std::move(codec);
            return true;
        }
    }
    codecs.push_back(std::move(codec));
    return true;
}

void unregister_component_codec(World& world, StringView name) {
    std::vector<ComponentCodec>& codecs = detail::context(world.registry()).codecs;
    std::erase_if(codecs, [&](const ComponentCodec& c) { return c.name == name; });
}

Result<std::string> save_scene_to_string(const World& world) {
    return write_document(world, scene_order(world), "scene");
}

Result<void> save_scene(const World& world, const std::filesystem::path& file) {
    return write_text_atomic(file, write_document(world, scene_order(world), "scene"));
}

Result<SceneLoadResult> load_scene_from_string(World& world, StringView json,
                                               const SceneLoadOptions& options) {
    Json        doc;
    const Json* entities = nullptr;
    if (auto parsed = parse_document(json, doc, entities); !parsed) {
        return parsed.error();
    }
    if (options.clear_world) {
        world.clear(); // only after the document validated
    }
    return instantiate(world, *entities, options.keep_uuids, kNullEntity);
}

Result<SceneLoadResult> load_scene(World& world, const std::filesystem::path& file,
                                   const SceneLoadOptions& options) {
    auto text = read_text(file);
    if (!text) {
        return text.error();
    }
    auto result = load_scene_from_string(world, *text, options);
    if (result) {
        AE_LOG_INFO(kCat, "loaded scene '{}': {} entities, {} warnings", display(file),
                    result->entity_count, result->warning_count);
    }
    return result;
}

Result<std::string> save_entities_to_string(const World& world, std::span<const Entity> roots) {
    std::vector<Entity> kept;
    for (const Entity r : roots) {
        if (!world.valid(r) || std::find(kept.begin(), kept.end(), r) != kept.end()) {
            continue;
        }
        bool nested = false;
        for (const Entity other : roots) {
            if (other != r && world.valid(other) && is_ancestor(world, other, r)) {
                nested = true; // folded into the ancestor's subtree
                break;
            }
        }
        if (!nested) {
            kept.push_back(r);
        }
    }
    if (kept.empty()) {
        return make_error<std::string>(ErrorCode::InvalidArgument,
                                       "scene: no valid entities to serialize");
    }
    std::vector<Entity> order;
    for (const Entity r : kept) {
        order.push_back(r);
        for_each_descendant(world, r, [&](Entity d) { order.push_back(d); });
    }
    return write_document(world, order, "entities");
}

Result<std::string> save_subtree_to_string(const World& world, Entity root) {
    return save_entities_to_string(world, std::span<const Entity>(&root, 1));
}

Result<void> save_subtree(const World& world, Entity root, const std::filesystem::path& file) {
    auto text = save_subtree_to_string(world, root);
    if (!text) {
        return text.error();
    }
    return write_text_atomic(file, *text);
}

Result<std::vector<Entity>> load_entities_from_string(World& world, StringView json, Entity parent) {
    if (parent != kNullEntity && !world.valid(parent)) {
        return make_error<std::vector<Entity>>(ErrorCode::InvalidArgument,
                                               "scene: paste target parent is not a valid entity");
    }
    Json        doc;
    const Json* entities = nullptr;
    if (auto parsed = parse_document(json, doc, entities); !parsed) {
        return parsed.error();
    }
    SceneLoadResult result = instantiate(world, *entities, /*keep_uuids=*/false, parent);
    return std::move(result.roots);
}

Result<Entity> load_subtree_from_string(World& world, StringView json, Entity parent) {
    auto roots = load_entities_from_string(world, json, parent);
    if (!roots) {
        return roots.error();
    }
    if (roots->empty()) {
        return make_error<Entity>(ErrorCode::NotFound, "scene: document contains no entities");
    }
    return roots->front();
}

Result<Entity> load_subtree(World& world, const std::filesystem::path& file, Entity parent) {
    auto text = read_text(file);
    if (!text) {
        return text.error();
    }
    return load_subtree_from_string(world, *text, parent);
}

} // namespace aether::scene
