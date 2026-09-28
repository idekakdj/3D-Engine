// editor_app.cpp — EditorApp lifecycle, viewport rendering, editor camera and actions.
#include "editor_app.h"
#include "thumbnail_cache.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/input.h"
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/editor/console.h"
#include "aether/editor/material_instance.h"
#include "aether/editor/prefab.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/debug_ui.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/gameplay/scene_instantiation.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device.h"
#include "aether/rhi/imgui.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"
#include "aether/scripting/components.h"

#include <imgui.h> // before ImGuizmo.h, which does not include it
#include <ImGuizmo.h>

#include <algorithm>
#include <cctype>

namespace aether::editor {

namespace {
constexpr f32 kNear = 0.1f;
constexpr f32 kFar  = 2000.0f;

std::string lower_ext(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return e;
}
} // namespace

EditorApp::EditorApp(gameplay::AppDesc desc, EditorOptions options)
    : Application(std::move(desc)), options_(options),
      history_([this](World&, const std::string& snapshot) -> Result<void> {
          return reload_world([&](World& w) -> Result<void> {
              auto r = scene::load_scene_from_string(w, snapshot);
              if (!r) {
                  return r.error();
              }
              return {};
          });
      }) {
    camera_input_.add_default_camera_bindings();
    fly_.require_look_button = true;
    fly_.move_speed          = 8.0f;
}

EditorApp::~EditorApp() = default; // ThumbnailCache is complete here (unique_ptr)

// =================================================================================================
// lifecycle
// =================================================================================================
Result<void> EditorApp::on_init() {
    install_console_sink();
    set_simulation_enabled(false); // edit mode: physics / scripts / animation are paused
    register_editor_codecs(world());
    pick_tracker_.set_timeout_frames(device().frames_in_flight() + 2);

    rhi::SamplerDesc sd;
    sd.address_u = sd.address_v = sd.address_w = rhi::AddressMode::ClampToEdge;
    viewport_sampler_ = device().create_sampler(sd);
    thumbnails_ = std::make_unique<ThumbnailCache>(device(), assets(), content_root(), viewport_sampler_,
                                                   renderer().settings().shadows);

    if (world().entity_count() == 0) {
        new_scene();
    } else {
        scene_path_ = desc().startup_scene.is_absolute() ? desc().startup_scene : content_root() / desc().startup_scene;
        // The Application loaded the startup scene before the editor codecs existed: reload it so
        // prefab links and material instances are not dropped.
        if (!open_scene(scene_path_)) {
            history_.mark_clean(world());
        }
    }
    set_editor_camera(Vec3(6.0f, 5.0f, 10.0f), Vec3(0.0f, 0.5f, 0.0f));
    if (!options_.select.empty()) {
        const Entity e = scene::find_by_name(world(), options_.select);
        if (e != kNullEntity) {
            select(e);
        } else {
            AE_LOG_WARN("Editor", "--select: no entity named '{}'", options_.select);
        }
    }
    if (!options_.browse.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(content_root() / options_.browse, ec)) {
            assets_dir_ = std::filesystem::path(options_.browse).lexically_normal();
        } else {
            AE_LOG_WARN("Editor", "--browse: no content folder '{}'", options_.browse);
        }
    }
    // Projects (ADR-0014): remember the open project for File > Recent Projects.
    recent_.load();
    if (!options_.project_file.empty()) {
        recent_.add(options_.project_file);
        if (auto r = recent_.save(); !r) {
            AE_LOG_WARN("Editor", "{}", r.error().message);
        }
    }
    if (options_.show_projects) {
        open_projects_window(false);
    }
    AE_LOG_INFO("Editor", "ready (content root {})", content_root().generic_string());
    return {};
}

void EditorApp::on_shutdown() {
    if (options_.self_test) {
        AE_LOG_INFO("Editor", "self-test {}", self_test_passed_ ? "PASSED" : "FAILED");
        for (const std::string& f : self_test_failures_) {
            AE_LOG_ERROR("Editor", "self-test failure: {}", f);
        }
    }
    device().wait_idle();
    if (thumbnails_) {
        thumbnails_->shutdown();
        thumbnails_.reset();
    }
    retire_viewport_targets(true);
    if (viewport_sampler_.is_valid()) {
        device().destroy(viewport_sampler_);
        viewport_sampler_ = {};
    }
}

void EditorApp::on_update(const FrameTime& time) {
    if (options_.self_test) {
        self_test_tick();
    }
    update_editor_camera(std::min(time.unscaled_delta, 0.1f));
    apply_camera_override();
    retire_viewport_targets(false);
    if (thumbnails_) {
        thumbnails_->update(time.frame_index);
    }
    poll_gpu_pick();
    sync_materials();
}

void EditorApp::on_imgui() {
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true; // viewport drags box-select, not move
    ImGuizmo::BeginFrame();
    draw_menu_bar();
    draw_dockspace();
    draw_toolbar();
    draw_hierarchy();
    draw_inspector();
    if (show_material_) {
        draw_material_editor();
    }
    if (show_animation_) {
        draw_animation_panel();
    }
    if (show_assets_) {
        draw_assets();
    }
    if (show_console_) {
        draw_console();
    }
    if (show_stats_) {
        gameplay::draw_engine_stats(*this, &show_stats_);
    }
    draw_viewport();
    draw_file_dialog();
    draw_projects_window();
    handle_shortcuts();

    // Deferred structural edits (never mutate the hierarchy while the tree is being drawn).
    if (!pending_reparent_.empty()) {
        bool any = false;
        for (const auto& [child, parent] : pending_reparent_) {
            const Entity c = scene::find_by_uuid(world(), child);
            const Entity p = parent != 0 ? scene::find_by_uuid(world(), parent) : kNullEntity;
            if (c == kNullEntity || (parent != 0 && p == kNullEntity)) {
                continue;
            }
            if (scene::set_parent_keep_world(world(), c, p)) {
                any = true;
            } else {
                AE_LOG_WARN("Editor", "cannot parent an entity to itself or its descendant");
            }
        }
        pending_reparent_.clear();
        if (any) {
            world().update_transforms();
            record_edit("Reparent");
        }
    }
    if (pending_delete_selection_) {
        pending_delete_selection_ = false;
        delete_selection();
    }
    for (const auto& [path, parent] : pending_asset_drops_) {
        const Entity p = parent != 0 ? scene::find_by_uuid(world(), parent) : kNullEntity;
        if (parent == 0 || p != kNullEntity) {
            drop_asset_on_entity(path, p);
        }
    }
    pending_asset_drops_.clear();
    for (const auto& [uuid, apply] : pending_prefab_ops_) {
        const Entity e = scene::find_by_uuid(world(), uuid);
        if (e != kNullEntity) {
            apply ? apply_prefab(e) : revert_prefab(e);
        }
    }
    pending_prefab_ops_.clear();
}

void EditorApp::on_render_frame(rhi::FrameInfo& frame, renderer::RenderScene& scene) {
    // ADR-0016: outline the selected GI volume (edit mode only).
    if (const Entity sel = selected(); play_state_ == PlayState::Edit && sel != kNullEntity && world().valid(sel)) {
        if (const auto* gv = world().try_get<gameplay::GIVolumeComponent>(sel)) {
            const renderer::GiVolume box = gameplay::gi_volume_from(world().world_matrix(sel), *gv);
            if (box.enabled) {
                const u32 col = gv->enabled ? 0xFFFFD040u : 0xFF808080u; // RGBA8, R in the low byte
                for (int i = 0; i < 12; ++i) {
                    // Edge i joins corners a and b (bit 0 = x, 1 = y, 2 = z) that differ in one axis.
                    const int axis = i / 4;
                    const int rest = i % 4;
                    const int lo = axis == 0 ? (rest & 1) << 1 | (rest & 2) << 1
                                   : axis == 1 ? (rest & 1) | (rest & 2) << 1
                                               : (rest & 1) | (rest & 2);
                    const int a = lo;
                    const int b = lo | (1 << axis);
                    auto corner = [&](int c) {
                        return Vec3((c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y,
                                    (c & 4) ? box.max.z : box.min.z);
                    };
                    scene.debug_lines.push_back(renderer::RenderLine{ corner(a), corner(b), col, col });
                }
            }
        }
    }
    if (viewport_.texture.is_valid() && viewport_.size.x > 0 && viewport_.size.y > 0) {
        renderer().resize(viewport_.size);
        scene.view.viewport = viewport_.size;
        renderer::RenderTarget target;
        target.texture       = viewport_.texture;
        target.format        = rhi::Format::RGBA8Unorm;
        target.extent        = viewport_.size;
        target.initial_state = rhi::ResourceState::Undefined;
        target.final_state   = rhi::ResourceState::ShaderRead; // sampled by ImGui below
        renderer().render(scene, *frame.cmd, target);
    }
    if (thumbnails_) {
        thumbnails_->render(*frame.cmd); // at most one asset thumbnail per frame (own renderer)
    }
    render_imgui_overlay(frame, /*clear_first=*/true);
}

// =================================================================================================
// viewport target
// =================================================================================================
void EditorApp::ensure_viewport_target(UVec2 size) {
    size = UVec2(std::max(size.x, 16u), std::max(size.y, 16u));
    if (viewport_.texture.is_valid() && viewport_.size == size) {
        return;
    }
    if (viewport_.texture.is_valid()) {
        viewport_.retire_frame = time().frame_index + device().frames_in_flight() + 1;
        retired_.push_back(viewport_);
        viewport_ = {};
    }
    rhi::TextureDesc td;
    td.type       = rhi::TextureType::Tex2D;
    td.format     = rhi::Format::RGBA8Unorm;
    td.width      = size.x;
    td.height     = size.y;
    td.usage      = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorAttach;
    td.debug_name = "EditorViewport";
    viewport_.texture = device().create_texture(td);
    if (!viewport_.texture.is_valid()) {
        AE_LOG_ERROR("Editor", "viewport target creation failed ({}x{})", size.x, size.y);
        return;
    }
    viewport_.imgui_id = rhi::imgui_add_texture(device(), viewport_.texture, viewport_sampler_);
    viewport_.size     = size;
    set_render_extent(size);
}

void EditorApp::retire_viewport_targets(bool all) {
    const u64 now = time().frame_index;
    std::erase_if(retired_, [&](const ViewportTarget& t) {
        if (!all && t.retire_frame > now) {
            return false;
        }
        if (t.imgui_id != 0) {
            rhi::imgui_remove_texture(device(), t.imgui_id);
        }
        device().destroy(t.texture);
        return true;
    });
    if (all && viewport_.texture.is_valid()) {
        if (viewport_.imgui_id != 0) {
            rhi::imgui_remove_texture(device(), viewport_.imgui_id);
        }
        device().destroy(viewport_.texture);
        viewport_ = {};
    }
}

// =================================================================================================
// camera
// =================================================================================================
void EditorApp::set_editor_camera(const Vec3& position, const Vec3& target) {
    camera_.position = position;
    const Vec3 dir   = target - position;
    if (glm::dot(dir, dir) > 1e-8f) {
        camera_.rotation = glm::quatLookAtRH(glm::normalize(dir), Vec3(0, 1, 0));
    }
    camera_.scale    = Vec3(1.0f);
    fly_.initialized = false;
}

Mat4 EditorApp::view_matrix() const {
    Transform rigid = camera_;
    rigid.scale     = Vec3(1.0f);
    return glm::inverse(rigid.to_matrix());
}

Mat4 EditorApp::projection_matrix() const {
    const f32 aspect = viewport_size_.y > 0.0f ? viewport_size_.x / viewport_size_.y : 16.0f / 9.0f;
    return perspective(fov_y_deg_ * kDeg2Rad, aspect, kNear, kFar);
}

std::optional<PickHit> EditorApp::pick(const Vec2& uv) {
    gameplay::RenderResourceCache* cache = render_cache();
    if (cache == nullptr) {
        return std::nullopt;
    }
    return pick_entity(world(), *cache, make_pick_ray(view_matrix(), projection_matrix(), uv));
}

void EditorApp::update_editor_camera(f32 dt) {
    const InputState raw = Input::state();
    const bool       rmb = raw.mouse_down(MouseButton::Right);
    looking_             = rmb && (looking_ || viewport_hovered_);
    if (looking_) {
        camera_input_.update(raw);
        gameplay::update_fly_camera(fly_, camera_, camera_input_, dt);
    } else {
        camera_input_.update(InputState{});
        if (viewport_hovered_ && raw.scroll.y != 0.0f) {
            camera_.position += (camera_.rotation * Vec3(0, 0, -1)) * raw.scroll.y * 1.5f;
        }
    }
}

void EditorApp::apply_camera_override() {
    const bool game_camera = play_state_ != PlayState::Edit && use_game_camera_ &&
                             gameplay::find_primary_camera(world()) != kNullEntity;
    if (game_camera) {
        set_camera_override(std::nullopt);
        return;
    }
    gameplay::CameraOverride o;
    o.view          = view_matrix();
    o.fov_y_radians = fov_y_deg_ * kDeg2Rad;
    o.near_z        = kNear;
    o.far_z         = kFar;
    set_camera_override(o);
}

void EditorApp::focus(Entity e) {
    if (!world().valid(e)) {
        return;
    }
    Vec3 center = scene::world_position(world(), e);
    f32  radius = 1.0f;
    if (const auto* mr = world().try_get<MeshRendererComponent>(e); mr != nullptr && render_cache() != nullptr) {
        if (const auto* mesh = render_cache()->mesh(mr->mesh)) {
            const AABB b = gameplay::transform_aabb(world().world_matrix(e), mesh->bounds);
            center       = b.center();
            radius       = std::max(glm::length(b.extent()), 0.25f);
        }
    }
    const Vec3 back = camera_.rotation * Vec3(0, 0, 1);
    set_editor_camera(center + back * (radius * 2.5f + 1.0f), center);
}

// =================================================================================================
// selection & history
// =================================================================================================
void EditorApp::select(Entity e) { select_click(e, SelectMode::Replace); }

void EditorApp::select_click(Entity e, SelectMode mode) {
    const u64 uuid = world().valid(e) ? scene::uuid_of(world(), e) : 0;
    selection_.apply_click(uuid, mode);
    if (world().valid(e)) {
        rename_buffer_ = world().get<NameComponent>(e).name;
    }
}

void EditorApp::select_entities(std::span<const Entity> entities) {
    std::vector<u64> uuids;
    for (const Entity e : entities) {
        if (world().valid(e)) {
            uuids.push_back(scene::uuid_of(world(), e));
        }
    }
    selection_.set_all(uuids);
}

Entity EditorApp::selected() {
    const u64 p = selection_.primary();
    return p != 0 ? scene::find_by_uuid(world(), p) : kNullEntity;
}

std::vector<Entity> EditorApp::selection() { return selection_.entities(world()); }

bool EditorApp::is_selected(Entity e) { return world().valid(e) && selection_.contains(scene::uuid_of(world(), e)); }

void EditorApp::record_edit(const char* label) {
    if (play_state_ == PlayState::Edit) {
        bind_material_instances(world()); // duplicates / instances get their own material ids
        history_.record(label, world());
    }
}

void EditorApp::flush_deferred_edits() {
    if (deferred_end_) {
        deferred_end_ = false;
        end_edit();
    }
    if (!deferred_commit_.empty()) {
        const std::string label = std::move(deferred_commit_);
        deferred_commit_.clear();
        record_edit(label.c_str());
    }
}
void EditorApp::begin_edit(const char* label) {
    if (play_state_ == PlayState::Edit) {
        history_.begin_continuous(label);
    }
}
void EditorApp::end_edit() {
    if (play_state_ == PlayState::Edit) {
        history_.end_continuous(world());
    }
}

void EditorApp::undo() {
    if (play_state_ != PlayState::Edit) {
        return;
    }
    if (const auto label = history_.undo(world())) {
        AE_LOG_INFO("Editor", "undo: {}", *label);
    }
}

void EditorApp::redo() {
    if (play_state_ != PlayState::Edit) {
        return;
    }
    if (const auto label = history_.redo(world())) {
        AE_LOG_INFO("Editor", "redo: {}", *label);
    }
}

void EditorApp::restore_snapshot(const std::string& snapshot) {
    auto r = reload_world([&](World& w) -> Result<void> {
        auto loaded = scene::load_scene_from_string(w, snapshot);
        if (!loaded) {
            return loaded.error();
        }
        return {};
    });
    if (!r) {
        AE_LOG_ERROR("Editor", "restoring the world failed: {}", r.error().message);
    }
}

// =================================================================================================
// scenes
// =================================================================================================
std::filesystem::path EditorApp::content_root() const {
    return desc().content_root.empty() ? paths::content_dir() : desc().content_root;
}

gameplay::RenderResourceCache* EditorApp::render_cache() {
    auto* bridge = find_subsystem<gameplay::RenderBridgeSubsystem>();
    return bridge != nullptr ? bridge->cache() : nullptr;
}

void EditorApp::new_scene() {
    if (play_state_ != PlayState::Edit) {
        stop();
    }
    auto r = reload_world([](World& w) -> Result<void> {
        const Entity sun = w.create("Sun");
        LightComponent l;
        l.kind         = LightKind::Directional;
        l.intensity    = 3.0f;
        l.cast_shadows = true;
        w.add<LightComponent>(sun, l);
        scene::set_local_rotation(w, sun, gameplay::rotation_from_yaw_pitch(30.0f, -55.0f));
        const Entity ground = w.create("Ground");
        MeshRendererComponent mr;
        mr.mesh     = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Plane);
        mr.material = gameplay::default_material_id();
        w.add<MeshRendererComponent>(ground, mr);
        return {};
    });
    (void)r;
    scene_path_.clear();
    selection_.clear();
    history_.clear();
    history_.mark_clean(world());
    AE_LOG_INFO("Editor", "new scene");
}

bool EditorApp::open_scene(const std::filesystem::path& file) {
    if (play_state_ != PlayState::Edit) {
        stop();
    }
    const std::filesystem::path path = file.is_absolute() ? file : content_root() / file;
    auto r = reload_world([&](World& w) -> Result<void> {
        auto loaded = scene::load_scene(w, path);
        if (!loaded) {
            return loaded.error();
        }
        return {};
    });
    if (!r) {
        AE_LOG_ERROR("Editor", "open {} failed: {}", path.generic_string(), r.error().message);
        return false;
    }
    scene_path_ = path;
    selection_.clear();
    history_.clear();
    history_.mark_clean(world());
    AE_LOG_INFO("Editor", "opened {} ({} entities)", path.generic_string(), world().entity_count());
    return true;
}

bool EditorApp::save_scene(const std::filesystem::path& file) {
    if (play_state_ != PlayState::Edit) {
        AE_LOG_WARN("Editor", "stop play mode before saving");
        return false;
    }
    const std::filesystem::path path = file.is_absolute() ? file : content_root() / file;
    if (auto r = scene::save_scene(world(), path); !r) {
        AE_LOG_ERROR("Editor", "save {} failed: {}", path.generic_string(), r.error().message);
        return false;
    }
    scene_path_ = path;
    history_.mark_saved();
    AE_LOG_INFO("Editor", "saved {}", path.generic_string());
    return true;
}

// =================================================================================================
// entities
// =================================================================================================
Entity EditorApp::create_entity(CreateKind kind, Entity parent) {
    World&      w    = world();
    const char* name = "Entity";
    switch (kind) {
    case CreateKind::Empty: name = "Empty"; break;
    case CreateKind::Cube: name = "Cube"; break;
    case CreateKind::Sphere: name = "Sphere"; break;
    case CreateKind::Plane: name = "Plane"; break;
    case CreateKind::Capsule: name = "Capsule"; break;
    case CreateKind::DirectionalLight: name = "Directional Light"; break;
    case CreateKind::PointLight: name = "Point Light"; break;
    case CreateKind::SpotLight: name = "Spot Light"; break;
    case CreateKind::Camera: name = "Camera"; break;
    case CreateKind::GIVolume: name = "GI Volume"; break;
    }
    const bool   has_parent = parent != kNullEntity && w.valid(parent);
    const Entity e          = has_parent ? w.create_child(parent, name) : w.create(name);
    if (!has_parent) {
        // In front of the editor camera, on the ground plane's side of the horizon.
        Vec3 p = camera_.position + (camera_.rotation * Vec3(0, 0, -1)) * 6.0f;
        scene::set_local_position(w, e, glm::round(p * 2.0f) * 0.5f);
    }
    auto mesh = [&](gameplay::BuiltinMesh m) {
        MeshRendererComponent mr;
        mr.mesh     = gameplay::builtin_mesh_id(m);
        mr.material = gameplay::default_material_id();
        w.add<MeshRendererComponent>(e, mr);
    };
    switch (kind) {
    case CreateKind::Empty: break;
    case CreateKind::Cube: mesh(gameplay::BuiltinMesh::Cube); break;
    case CreateKind::Sphere: mesh(gameplay::BuiltinMesh::Sphere); break;
    case CreateKind::Plane: mesh(gameplay::BuiltinMesh::Plane); break;
    case CreateKind::Capsule: mesh(gameplay::BuiltinMesh::Capsule); break;
    case CreateKind::DirectionalLight: {
        LightComponent l;
        l.kind      = LightKind::Directional;
        l.intensity = 3.0f;
        w.add<LightComponent>(e, l);
        scene::set_local_rotation(w, e, gameplay::rotation_from_yaw_pitch(0.0f, -45.0f));
        break;
    }
    case CreateKind::PointLight: {
        LightComponent l;
        l.kind      = LightKind::Point;
        l.intensity = 20.0f;
        w.add<LightComponent>(e, l);
        break;
    }
    case CreateKind::SpotLight: {
        LightComponent l;
        l.kind         = LightKind::Spot;
        l.intensity    = 30.0f;
        l.cast_shadows = true; // ADR-0012: spot shadows are cheap (one map per light, budgeted)
        w.add<LightComponent>(e, l);
        scene::set_local_rotation(w, e, gameplay::rotation_from_yaw_pitch(0.0f, -60.0f));
        break;
    }
    case CreateKind::Camera: {
        CameraComponent c;
        c.primary = gameplay::find_primary_camera(w) == kNullEntity;
        w.add<CameraComponent>(e, c);
        break;
    }
    case CreateKind::GIVolume: // ADR-0016: the entity's scale is the box size
        w.add<gameplay::GIVolumeComponent>(e);
        scene::set_local_scale(w, e, Vec3(16.0f, 6.0f, 16.0f));
        break;
    }
    w.update_transforms();
    record_edit("Create");
    select(e);
    return e;
}

void EditorApp::delete_entity(Entity e) {
    if (!world().valid(e)) {
        return;
    }
    world().destroy(e);
    selection_.prune(world());
    record_edit("Delete");
}

Entity EditorApp::duplicate_entity(Entity e) {
    if (!world().valid(e)) {
        return kNullEntity;
    }
    const Entity copy = scene::clone_entity(world(), e);
    if (copy == kNullEntity) {
        return kNullEntity;
    }
    world().get<NameComponent>(copy).name += " (copy)";
    world().update_transforms();
    record_edit("Duplicate");
    select(copy);
    return copy;
}

bool EditorApp::reparent(Entity child, Entity parent) {
    if (!world().valid(child) || (parent != kNullEntity && !world().valid(parent))) {
        return false;
    }
    if (!scene::set_parent_keep_world(world(), child, parent)) {
        AE_LOG_WARN("Editor", "cannot parent an entity to itself or its descendant");
        return false;
    }
    world().update_transforms();
    record_edit("Reparent");
    return true;
}

bool EditorApp::instantiate_asset(const std::filesystem::path& rel) {
    const std::string ext = lower_ext(rel);
    if (ext == ".aescene") {
        return open_scene(rel);
    }
    const Vec3 in_front = glm::round((camera_.position + (camera_.rotation * Vec3(0, 0, -1)) * 6.0f) * 2.0f) * 0.5f;
    if (ext == ".gltf" || ext == ".glb") {
        const Entity root = instantiate_model_at(rel, kNullEntity, in_front);
        if (root == kNullEntity) {
            return false;
        }
        record_edit("Instantiate model");
        select(root);
        return true;
    }
    if (ext == kPrefabExtension) {
        const auto roots = instantiate_prefab_file(rel, kNullEntity, glm::translate(Mat4(1.0f), in_front));
        if (roots.empty()) {
            return false;
        }
        record_edit("Instantiate prefab");
        select_entities(roots);
        return true;
    }
    if (ext == ".lua") {
        const Entity e = selected();
        if (e == kNullEntity) {
            AE_LOG_WARN("Editor", "select an entity to attach {} to", rel.generic_string());
            return false;
        }
        std::filesystem::path script = rel;
        if (!script.empty() && *script.begin() == "scripts") {
            script = script.lexically_relative("scripts"); // ScriptComponent paths are relative to content/scripts
        }
        scripting::ScriptComponent sc;
        sc.script = script.generic_string();
        world().add<scripting::ScriptComponent>(e, sc);
        record_edit("Attach script");
        return true;
    }
    AE_LOG_WARN("Editor", "don't know what to do with {}", rel.generic_string());
    return false;
}

// =================================================================================================
// play in editor
// =================================================================================================
void EditorApp::play() {
    if (play_state_ == PlayState::Paused) {
        toggle_pause();
        return;
    }
    if (play_state_ != PlayState::Edit) {
        return;
    }
    history_.end_continuous(world());
    auto snap = scene::save_scene_to_string(world());
    if (!snap) {
        AE_LOG_ERROR("Editor", "cannot enter play mode: {}", snap.error().message);
        return;
    }
    play_snapshot_ = std::move(*snap);
    play_state_    = PlayState::Playing;
    set_simulation_enabled(true);
    AE_LOG_INFO("Editor", "play");
}

void EditorApp::stop() {
    if (play_state_ == PlayState::Edit) {
        return;
    }
    set_simulation_enabled(false);
    play_state_ = PlayState::Edit;
    restore_snapshot(play_snapshot_);
    play_snapshot_.clear();
    history_.mark_clean(world());
    AE_LOG_INFO("Editor", "stop (world restored)");
}

void EditorApp::toggle_pause() {
    if (play_state_ == PlayState::Playing) {
        play_state_ = PlayState::Paused;
        set_simulation_enabled(false);
    } else if (play_state_ == PlayState::Paused) {
        play_state_ = PlayState::Playing;
        set_simulation_enabled(true);
    }
}

void EditorApp::step() {
    if (play_state_ == PlayState::Playing) {
        toggle_pause();
    }
    if (play_state_ == PlayState::Paused) {
        step_simulation_once();
    }
}

} // namespace aether::editor
