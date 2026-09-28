// editor_actions.cpp — M2 editor actions: selection operations and group transforms, GPU picking
// with CPU fallback, box select, prefabs, asset drag-and-drop and material instances.
#include "editor_app.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/editor/material_instance.h"
#include "aether/editor/prefab.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/gameplay/scene_instantiation.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"

#include <algorithm>
#include <cctype>

namespace aether::editor {

namespace {

std::string extension_of(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return e;
}

bool attach_script(World& w, Entity e, const std::filesystem::path& rel) {
    if (!w.valid(e)) {
        AE_LOG_WARN("Editor", "select an entity to attach {} to", rel.generic_string());
        return false;
    }
    std::filesystem::path script = rel;
    if (!script.empty() && *script.begin() == "scripts") {
        script = script.lexically_relative("scripts"); // ScriptComponent paths are relative to content/scripts
    }
    scripting::ScriptComponent sc;
    sc.script = script.generic_string();
    w.add<scripting::ScriptComponent>(e, sc);
    return true;
}

} // namespace

// =================================================================================================
// selection operations
// =================================================================================================
std::vector<Entity> EditorApp::duplicate_selection() {
    const std::vector<Entity> roots = selection_roots(world(), selection());
    std::vector<Entity>       copies;
    for (const Entity e : roots) {
        const Entity copy = scene::clone_entity(world(), e);
        if (copy != kNullEntity) {
            world().get<NameComponent>(copy).name += " (copy)";
            copies.push_back(copy);
        }
    }
    if (copies.empty()) {
        return copies;
    }
    world().update_transforms();
    record_edit(copies.size() > 1 ? "Duplicate selection" : "Duplicate");
    select_entities(copies);
    return copies;
}

void EditorApp::delete_selection() {
    const std::vector<Entity> roots = selection_roots(world(), selection());
    if (roots.empty()) {
        return;
    }
    for (const Entity e : roots) {
        world().destroy(e);
    }
    selection_.prune(world());
    record_edit(roots.size() > 1 ? "Delete selection" : "Delete");
}

bool EditorApp::reparent_selection(Entity parent) {
    if (parent != kNullEntity && !world().valid(parent)) {
        return false;
    }
    bool any = false;
    for (const Entity e : selection_roots(world(), selection())) {
        if (e == parent || (parent != kNullEntity && scene::is_ancestor(world(), e, parent))) {
            AE_LOG_WARN("Editor", "cannot parent an entity to itself or its descendant");
            continue;
        }
        any |= scene::set_parent_keep_world(world(), e, parent);
    }
    if (any) {
        world().update_transforms();
        record_edit("Reparent");
    }
    return any;
}

Mat4 EditorApp::selection_pivot_matrix() {
    const std::vector<Entity> roots = selection_roots(world(), selection());
    if (roots.empty()) {
        return Mat4(1.0f);
    }
    if (roots.size() == 1) {
        return world().world_matrix(roots.front()); // single entity: its own frame (scale included)
    }
    const Entity      primary     = selected();
    usize             primary_idx = roots.size() - 1;
    std::vector<Mat4> worlds;
    worlds.reserve(roots.size());
    for (usize i = 0; i < roots.size(); ++i) {
        worlds.push_back(world().world_matrix(roots[i]));
        if (roots[i] == primary || scene::is_ancestor(world(), roots[i], primary)) {
            primary_idx = i;
        }
    }
    return editor::selection_pivot(worlds, primary_idx, pivot_mode_, gizmo_local_);
}

bool EditorApp::transform_selection(const Mat4& pivot_now) {
    const std::vector<Entity> roots = selection_roots(world(), selection());
    if (roots.empty()) {
        return false;
    }
    world().update_transforms();
    const Mat4 start  = selection_pivot_matrix();
    const Mat4 target = snap_.translate ? snap_translation(pivot_now, snap_.translate_step) : pivot_now;
    std::vector<std::pair<Entity, Mat4>> starts;
    for (const Entity e : roots) {
        starts.emplace_back(e, world().world_matrix(e));
    }
    for (const auto& [e, w] : starts) {
        scene::set_world_matrix(world(), e, apply_pivot_delta(start, target, w));
    }
    world().update_transforms();
    record_edit("Transform");
    return true;
}

// =================================================================================================
// picking
// =================================================================================================
void EditorApp::request_viewport_pick(const Vec2& uv, SelectMode mode) {
    pending_pick_.ray  = make_pick_ray(view_matrix(), projection_matrix(), uv);
    pending_pick_.mode = mode;
    const UVec2 size   = viewport_.size;
    if (size.x == 0 || size.y == 0) {
        resolve_pick({ GpuPickTracker::Source::CpuFallback, 0u, 0u });
        last_pick_source_ = "cpu";
        return;
    }
    const UVec2 pixel(std::min(static_cast<u32>(std::max(uv.x, 0.0f) * static_cast<f32>(size.x)), size.x - 1),
                      std::min(static_cast<u32>(std::max(uv.y, 0.0f) * static_cast<f32>(size.y)), size.y - 1));
    const GpuPickTracker::Route route = pick_tracker_.request(time().frame_index);
    renderer().request_pick(pixel); // the pick, or a probe when resolving on the CPU below
    if (route == GpuPickTracker::Route::Cpu) {
        resolve_pick({ GpuPickTracker::Source::CpuFallback, 0u, 0u });
        last_pick_source_ = "cpu";
    }
}

void EditorApp::poll_gpu_pick() {
    const std::optional<u32> answer = renderer().poll_pick();
    if (const auto outcome = pick_tracker_.update(time().frame_index, answer)) {
        resolve_pick(*outcome);
    }
}

void EditorApp::resolve_pick(const GpuPickTracker::Outcome& outcome) {
    Entity hit = kNullEntity;
    if (outcome.source == GpuPickTracker::Source::Gpu) {
        hit               = entity_from_pick_id(world(), outcome.user_id);
        last_pick_source_ = "gpu";
    } else {
        if (gameplay::RenderResourceCache* cache = render_cache()) {
            if (const auto h = pick_entity(world(), *cache, pending_pick_.ray)) {
                hit = h->entity;
            }
        }
        last_pick_source_ = "cpu-fallback";
    }
    select_click(hit, pending_pick_.mode);
}

void EditorApp::box_select(const Vec2& uv_a, const Vec2& uv_b, SelectMode mode) {
    gameplay::RenderResourceCache* cache = render_cache();
    if (cache == nullptr) {
        return;
    }
    const std::vector<Entity> hits = entities_in_rect(world(), *cache, projection_matrix() * view_matrix(), uv_a, uv_b);
    if (mode == SelectMode::Replace) {
        select_entities(hits);
        return;
    }
    for (const Entity e : hits) {
        select_click(e, mode);
    }
}

// =================================================================================================
// prefabs
// =================================================================================================
std::filesystem::path EditorApp::resolve_content_path(const std::filesystem::path& p) const {
    return p.is_absolute() ? p : content_root() / p;
}

std::string EditorApp::content_relative_string(const std::filesystem::path& p) const {
    const std::filesystem::path abs = resolve_content_path(p).lexically_normal();
    const std::filesystem::path rel = abs.lexically_relative(content_root().lexically_normal());
    if (rel.empty() || *rel.begin() == "..") {
        return abs.generic_string();
    }
    return rel.generic_string();
}

bool EditorApp::create_prefab(const std::filesystem::path& file) {
    if (play_state_ != PlayState::Edit) {
        AE_LOG_WARN("Editor", "stop play mode before creating a prefab");
        return false;
    }
    const std::vector<Entity> roots = selection_roots(world(), selection());
    if (roots.empty()) {
        AE_LOG_WARN("Editor", "select the entities to turn into a prefab");
        return false;
    }
    world().update_transforms();
    std::vector<Mat4> worlds;
    for (const Entity e : roots) {
        worlds.push_back(world().world_matrix(e));
    }
    // Pivot: the selection's bounds centre, world-aligned (rotation/scale stay in the prefab).
    const Mat4 pivot = editor::selection_pivot(worlds, worlds.size() - 1, PivotMode::BoundsCenter, false);
    std::filesystem::path path = resolve_content_path(file);
    if (path.extension().empty()) {
        path += std::string(kPrefabExtension);
    }
    if (auto r = write_prefab(world(), roots, pivot, path); !r) {
        AE_LOG_ERROR("Editor", "create prefab {} failed: {}", path.generic_string(), r.error().message);
        return false;
    }
    const std::string source = content_relative_string(path);
    for (usize i = 0; i < roots.size(); ++i) {
        world().add<PrefabInstanceComponent>(roots[i], PrefabInstanceComponent{ source, static_cast<u32>(i) });
    }
    record_edit("Create prefab");
    AE_LOG_INFO("Editor", "created prefab {} ({} root(s))", source, roots.size());
    return true;
}

std::vector<Entity> EditorApp::instantiate_prefab_file(const std::filesystem::path& file, Entity parent, const Mat4& placement) {
    const std::filesystem::path path = resolve_content_path(file);
    auto roots = editor::instantiate_prefab(world(), path, content_relative_string(path), parent, placement);
    if (!roots) {
        AE_LOG_ERROR("Editor", "instantiate prefab {} failed: {}", path.generic_string(), roots.error().message);
        return {};
    }
    return std::move(*roots);
}

bool EditorApp::apply_prefab(Entity instance) {
    const auto* link = world().valid(instance) ? world().try_get<PrefabInstanceComponent>(instance) : nullptr;
    if (link == nullptr || play_state_ != PlayState::Edit) {
        return false;
    }
    const std::string           source = link->source;
    const std::filesystem::path path   = resolve_content_path(source);
    if (auto r = apply_prefab_instance(world(), instance, path); !r) {
        AE_LOG_ERROR("Editor", "apply prefab {} failed: {}", source, r.error().message);
        return false;
    }
    // Propagate to the other instances of the same prefab (they keep their root transforms).
    std::vector<Entity> others;
    for (const auto [e, pi] : world().registry().view<const PrefabInstanceComponent>().each()) {
        if (e != instance && pi.source == source) {
            others.push_back(e);
        }
    }
    for (const Entity e : others) {
        const u64 old_uuid = scene::uuid_of(world(), e);
        if (auto fresh = revert_prefab_instance(world(), e, path); fresh && selection_.remove(old_uuid)) {
            selection_.add(scene::uuid_of(world(), *fresh));
        }
    }
    record_edit("Apply prefab");
    AE_LOG_INFO("Editor", "applied {} ({} other instance(s) updated)", source, others.size());
    return true;
}

bool EditorApp::revert_prefab(Entity instance) {
    const auto* link = world().valid(instance) ? world().try_get<PrefabInstanceComponent>(instance) : nullptr;
    if (link == nullptr || play_state_ != PlayState::Edit) {
        return false;
    }
    const std::string source   = link->source;
    const u64         old_uuid = scene::uuid_of(world(), instance);
    auto              fresh    = revert_prefab_instance(world(), instance, resolve_content_path(source));
    if (!fresh) {
        AE_LOG_ERROR("Editor", "revert prefab {} failed: {}", source, fresh.error().message);
        return false;
    }
    if (selection_.remove(old_uuid)) {
        selection_.add(scene::uuid_of(world(), *fresh));
    }
    record_edit("Revert prefab");
    return true;
}

// =================================================================================================
// asset drag-and-drop
// =================================================================================================
Vec3 EditorApp::placement_point(const Vec2& uv) {
    const Ray          ray = make_pick_ray(view_matrix(), projection_matrix(), uv);
    std::optional<f32> t;
    if (gameplay::RenderResourceCache* cache = render_cache()) {
        if (const auto hit = pick_entity(world(), *cache, ray); hit && hit->distance > 0.0f) {
            t = hit->distance; // surface (world bounds) under the cursor
        }
    }
    if (!t) {
        t = ray_plane(ray, Vec3(0.0f, 1.0f, 0.0f), 0.0f); // ground plane
    }
    if (!t || *t > 500.0f) {
        t = 6.0f; // looking at the sky: in front of the camera
    }
    const Vec3 p = ray.origin + ray.direction * *t;
    return snap_.translate ? snap_position(p, snap_.translate_step) : p;
}

Entity EditorApp::instantiate_model_at(const std::filesystem::path& rel, Entity parent, const std::optional<Vec3>& position) {
    gameplay::SceneInstantiateOptions options;
    options.parent = parent;
    auto inst      = gameplay::instantiate_model(assets(), world(), rel.generic_string(), options);
    if (!inst) {
        AE_LOG_ERROR("Editor", "{}", inst.error().message);
        return kNullEntity;
    }
    if (position) {
        scene::set_local_position(world(), inst->root, *position);
    }
    world().update_transforms();
    return inst->root;
}

bool EditorApp::drop_asset_in_viewport(const std::filesystem::path& rel, const Vec2& uv) {
    const std::string ext = extension_of(rel);
    if (ext == ".lua" || ext == ".aegraph") { // ADR-0018: graphs attach like scripts
        const Ray ray = make_pick_ray(view_matrix(), projection_matrix(), uv);
        Entity    target = kNullEntity;
        if (gameplay::RenderResourceCache* cache = render_cache()) {
            if (const auto hit = pick_entity(world(), *cache, ray)) {
                target = hit->entity;
            }
        }
        if (!attach_script(world(), target, rel)) {
            return false;
        }
        record_edit("Attach script");
        select(target);
        return true;
    }
    const Vec3 point = placement_point(uv);
    if (ext == ".gltf" || ext == ".glb") {
        const Entity root = instantiate_model_at(rel, kNullEntity, point);
        if (root == kNullEntity) {
            return false;
        }
        record_edit("Drop model");
        select(root);
        return true;
    }
    std::vector<Entity> roots;
    if (ext == kPrefabExtension) {
        roots = instantiate_prefab_file(rel, kNullEntity, glm::translate(Mat4(1.0f), point));
    } else if (ext == ".aescene") {
        auto text = read_text_file(resolve_content_path(rel));
        auto r    = text ? instantiate_prefab_document(world(), *text, {}, kNullEntity, glm::translate(Mat4(1.0f), point))
                         : Result<std::vector<Entity>>(text.error());
        if (!r) {
            AE_LOG_ERROR("Editor", "drop {} failed: {}", rel.generic_string(), r.error().message);
            return false;
        }
        roots = std::move(*r);
    } else {
        AE_LOG_WARN("Editor", "cannot drop {} into the viewport", rel.generic_string());
        return false;
    }
    if (roots.empty()) {
        return false;
    }
    record_edit(ext == ".aescene" ? "Drop scene" : "Drop prefab");
    select_entities(roots);
    return true;
}

bool EditorApp::drop_asset_on_entity(const std::filesystem::path& rel, Entity parent) {
    const std::string ext = extension_of(rel);
    if (ext == ".lua" || ext == ".aegraph") { // ADR-0018: graphs attach like scripts
        if (!attach_script(world(), parent, rel)) {
            return false;
        }
        record_edit("Attach script");
        return true;
    }
    if (ext == ".gltf" || ext == ".glb") {
        const Entity root = instantiate_model_at(rel, parent, std::nullopt);
        if (root == kNullEntity) {
            return false;
        }
        record_edit("Drop model");
        select(root);
        return true;
    }
    std::vector<Entity> roots;
    if (ext == kPrefabExtension) {
        roots = instantiate_prefab_file(rel, parent, Mat4(1.0f));
    } else if (ext == ".aescene") {
        auto text = read_text_file(resolve_content_path(rel));
        auto r    = text ? instantiate_prefab_document(world(), *text, {}, parent, Mat4(1.0f))
                         : Result<std::vector<Entity>>(text.error());
        if (!r) {
            AE_LOG_ERROR("Editor", "drop {} failed: {}", rel.generic_string(), r.error().message);
            return false;
        }
        roots = std::move(*r);
    } else {
        AE_LOG_WARN("Editor", "cannot drop {} onto an entity", rel.generic_string());
        return false;
    }
    if (roots.empty()) {
        return false;
    }
    record_edit(ext == ".aescene" ? "Drop scene" : "Drop prefab");
    select_entities(roots);
    return true;
}

// =================================================================================================
// material instances
// =================================================================================================
assets::MaterialData EditorApp::material_data_for(const AssetId& id) {
    if (const auto builtin = gameplay::builtin_material_from_id(id)) {
        return gameplay::make_builtin_material(*builtin);
    }
    if (!id.is_valid()) {
        return gameplay::make_default_material();
    }
    if (is_material_instance_id(id)) {
        for (const auto [e, mi] : world().registry().view<const MaterialInstanceComponent>().each()) {
            const u64 uuid = scene::uuid_of(world(), e);
            for (const auto& s : mi.slots) {
                if (material_instance_id(uuid, s.slot) == id) {
                    return s.data;
                }
            }
        }
        return gameplay::make_default_material();
    }
    auto ref = assets().load_sync<assets::MaterialData>(id);
    if (ref.ready() && ref.get() != nullptr) {
        return *ref.get();
    }
    return gameplay::make_default_material();
}

void EditorApp::make_material_instance(Entity e, i32 slot) {
    if (!world().valid(e) || !world().has<MeshRendererComponent>(e)) {
        return;
    }
    AssetId current = material_slot_target(world(), e, slot);
    if (slot >= 0 && !current.is_valid()) {
        current = world().get<MeshRendererComponent>(e).material; // unset override: what the slot shows
    }
    assets::MaterialData data = material_data_for(current);
    if (!is_material_instance_id(current)) {
        data.name = (data.name.empty() ? std::string("Material") : data.name) + " (instance)";
    }
    editor::make_material_instance(world(), e, slot, data);
    record_edit("Create material instance");
    sync_materials();
}

void EditorApp::edit_material(Entity e, i32 slot, const assets::MaterialData& data, bool continuous) {
    auto* mi = world().valid(e) ? world().try_get<MaterialInstanceComponent>(e) : nullptr;
    auto* s  = mi != nullptr ? mi->find(slot) : nullptr;
    if (s == nullptr) {
        return;
    }
    if (continuous) {
        begin_edit("Material");
    }
    s->data = data;
    sync_materials(); // live preview
    if (!continuous) {
        record_edit("Material");
    }
}

void EditorApp::sync_materials() {
    bind_material_instances(world());
    gameplay::RenderResourceCache* cache = render_cache();
    if (cache == nullptr) {
        return;
    }
    for (const auto [e, mi] : world().registry().view<const MaterialInstanceComponent>().each()) {
        const u64 uuid = scene::uuid_of(world(), e);
        for (const auto& s : mi.slots) {
            const AssetId id   = material_instance_id(uuid, s.slot);
            const u64     hash = material_data_hash(s.data);
            const auto    it   = registered_materials_.find(id);
            if (it == registered_materials_.end() || it->second != hash) {
                cache->add_material(id, s.data);
                registered_materials_[id] = hash;
            }
        }
    }
}

} // namespace aether::editor
