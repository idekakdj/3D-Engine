// editor_app.h — PRIVATE: the Aether editor application (Layer 5).
//
// A gameplay::Application whose frame renders the world into an offscreen viewport texture
// shown in a docked ImGui window, surrounded by the hierarchy, inspector, asset browser and
// console panels. Editing happens with the simulation paused; Play snapshots the world and runs
// it, Stop restores the snapshot (play-in-editor). Every edit goes through EditHistory
// (snapshot undo/redo). The (multi-)selection is kept by entity uuid so it survives undo and
// reloads; operations on it (group transform, duplicate, delete, reparent, multi-edit) are one
// undo step each.
//
// M2 (ADR-0009 section 3): GPU id-buffer picking with CPU fallback, multi-select + group gizmo,
// grid and snapping, prefab assets (.aeprefab), asset drag-and-drop into the viewport /
// hierarchy, a material editor (scene-stored material instances) and a read-only animation view.
//
// The panel implementations live in panel_*.cpp; actions (create/delete/save/play...) in
// editor_app.cpp; the scripted --self-test in self_test.cpp.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/editor/edit_history.h"
#include "aether/editor/picking.h"
#include "aether/editor/selection.h"
#include "aether/editor/snapping.h"
#include "aether/gameplay/application.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/input_map.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/renderer/render_scene.h"
#include "aether/rhi/resources.h"
#include "aether/runtime/workspace.h"
#include "aether/scene/entity.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aether::gameplay {
class RenderResourceCache;
}

struct ImGuiStyle;

namespace aether::editor {

class ThumbnailCache;
class GraphEditor;

struct EditorOptions {
    bool        self_test = false; // run the scripted self-test and exit with its result
    std::string select;            // entity name to select (and focus) after startup
    std::string browse;            // content-relative folder the asset browser opens in
    std::filesystem::path project_file; // the open project's manifest (empty: the engine content folder)
    std::string           project_name;
    bool                  show_projects = false; // open the Projects window at startup
};

enum class CreateKind : u8 {
    Empty = 0,
    Cube,
    Sphere,
    Plane,
    Capsule,
    DirectionalLight,
    PointLight,
    SpotLight,
    Camera,
    GIVolume, // ADR-0016
    ReflectionProbe, // ADR-0017
};

enum class PlayState : u8 { Edit = 0, Playing, Paused };

class EditorApp final : public gameplay::Application {
public:
    EditorApp(gameplay::AppDesc desc, EditorOptions options);
    ~EditorApp() override;

    [[nodiscard]] bool self_test_passed() const noexcept { return self_test_passed_; }

    // ---- actions (menus, shortcuts, self-test) --------------------------------------------------
    void   new_scene();
    bool   open_scene(const std::filesystem::path& file);
    bool   save_scene(const std::filesystem::path& file);
    Entity create_entity(CreateKind kind, Entity parent = kNullEntity);
    void   delete_entity(Entity e);
    Entity duplicate_entity(Entity e);
    bool   reparent(Entity child, Entity parent); // keeps the world transform
    bool   instantiate_asset(const std::filesystem::path& content_relative);
    // Drag-and-drop targets (the UI and the self-test share these paths). Viewport: models,
    // prefabs and scenes are placed at the surface / ground-plane point under `viewport_uv`
    // (scripts attach to the entity there); entity: instantiated as children (scripts attach).
    bool   drop_asset_in_viewport(const std::filesystem::path& content_relative, const Vec2& viewport_uv);
    bool   drop_asset_on_entity(const std::filesystem::path& content_relative, Entity parent);
    [[nodiscard]] Vec3 placement_point(const Vec2& viewport_uv); // snapped when translate snap is on
    void   undo();
    void   redo();
    void   play();
    void   stop();
    void   toggle_pause();
    void   step();
    void   focus(Entity e);

    // ---- selection ------------------------------------------------------------------------------
    void                   select(Entity e); // replace (kNullEntity clears)
    void                   select_click(Entity e, SelectMode mode);
    void                   select_entities(std::span<const Entity> entities); // last = primary
    [[nodiscard]] Entity   selected();       // the primary (last selected) entity
    [[nodiscard]] std::vector<Entity> selection(); // valid entities, oldest first
    [[nodiscard]] bool     is_selected(Entity e);
    [[nodiscard]] const SelectionSet& selection_set() const noexcept { return selection_; }
    std::vector<Entity>    duplicate_selection();             // one undo step; selects the copies
    void                   delete_selection();                // one undo step
    bool                   reparent_selection(Entity parent); // keeps world transforms; one step
    // Moves the selection roots rigidly so the pivot becomes `pivot_now` (translation snapped when
    // translate snapping is on). One undo step. Returns false when nothing is selected.
    bool                   transform_selection(const Mat4& pivot_now);
    [[nodiscard]] Mat4     selection_pivot_matrix();
    [[nodiscard]] SnapSettings& snap_settings() noexcept { return snap_; }

    // ---- picking ----------------------------------------------------------------------------------
    // Async GPU pick (Renderer::request_pick) resolved over the next frames, CPU fallback on
    // timeout / no support. The click's selection mode is applied when it resolves.
    void request_viewport_pick(const Vec2& viewport_uv, SelectMode mode);
    [[nodiscard]] const GpuPickTracker& pick_tracker() const noexcept { return pick_tracker_; }
    void box_select(const Vec2& uv_a, const Vec2& uv_b, SelectMode mode);

    // ---- prefabs --------------------------------------------------------------------------------
    // Writes the selection roots as a prefab (relative to the selection pivot) and links them to
    // it. One undo step (the link).
    bool create_prefab(const std::filesystem::path& file);
    std::vector<Entity> instantiate_prefab_file(const std::filesystem::path& file, Entity parent, const Mat4& placement);
    bool apply_prefab(Entity instance);
    bool revert_prefab(Entity instance);
    [[nodiscard]] std::filesystem::path resolve_content_path(const std::filesystem::path& p) const;
    [[nodiscard]] std::string           content_relative_string(const std::filesystem::path& p) const;

    // ---- materials ------------------------------------------------------------------------------
    // Converts a slot of `e` (-1 = the mesh renderer's material, >= 0 = override slot) into an
    // editable instance initialised from what it currently shows. One undo step.
    void make_material_instance(Entity e, i32 slot);
    // Sets an instance slot's parameters (live preview). continuous = part of a drag (close it
    // with end_material_edit()); otherwise one undo step.
    void edit_material(Entity e, i32 slot, const assets::MaterialData& data, bool continuous);
    void end_material_edit() { end_edit(); }
    // Re-binds instance references and (re-)registers changed instance materials with the cache.
    void sync_materials();
    [[nodiscard]] assets::MaterialData material_data_for(const AssetId& id);
    [[nodiscard]] PlayState play_state() const noexcept { return play_state_; }

    // ---- projects (ADR-0014) --------------------------------------------------------------------
    // Reopens the editor on `manifest` (a new editor process; this one exits). Unsaved scene
    // changes must have been handled by the caller (the UI asks). Returns false if the launch failed.
    bool switch_project(const std::filesystem::path& manifest);
    // Creates a project and switches to it.
    bool create_and_open_project(const std::filesystem::path& parent, const std::string& name,
                                 runtime::ProjectTemplate tmpl);
    [[nodiscard]] runtime::RecentProjects& recent_projects() noexcept { return recent_; }
    void open_projects_window(bool new_tab);
    // Replaces the process launcher (self-test: record instead of starting a second editor).
    using Launcher = std::function<Result<void>(const std::filesystem::path&, const std::vector<std::string>&)>;
    void set_launcher(Launcher l) { launcher_ = std::move(l); }
    // Asset-browser thumbnails (null before on_init / after shutdown).
    [[nodiscard]] ThumbnailCache* thumbnails() noexcept { return thumbnails_.get(); }
    // ADR-0018: visual scripts. open_graph takes a content-relative path ("scripts/door.aegraph").
    bool open_graph(const std::filesystem::path& content_rel);
    // Creates scripts/<entity name>.aegraph (starter graph), attaches it to `e` and opens it.
    bool new_graph_for(Entity e);
    [[nodiscard]] GraphEditor* graph_editor() noexcept { return graph_editor_.get(); }
    [[nodiscard]] EditHistory& history() noexcept { return history_; }

    // Editor camera (world-space pose) and the matrices the viewport renders with.
    void                   set_editor_camera(const Vec3& position, const Vec3& target);
    [[nodiscard]] Mat4     view_matrix() const;
    [[nodiscard]] Mat4     projection_matrix() const; // engine convention (Y-flipped, depth 0..1)
    [[nodiscard]] std::optional<PickHit> pick(const Vec2& viewport_uv);

protected:
    Result<void> on_init() override;
    void         on_shutdown() override;
    void         on_update(const FrameTime& time) override;
    void         on_imgui() override;
    void         on_render_frame(rhi::FrameInfo& frame, renderer::RenderScene& scene) override;

private:
    // ---- panels (panel_*.cpp) -------------------------------------------------------------------
    void draw_dockspace();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_viewport();
    void draw_hierarchy();
    void draw_inspector();
    void draw_assets();
    void draw_console();
    void draw_file_dialog();
    void draw_projects_window();
    void draw_material_editor();
    void draw_animation_panel();
    void draw_viewport_gizmo(const Vec2& origin, bool game_view, bool& gizmo_hover);
    void handle_shortcuts();

    // ---- helpers --------------------------------------------------------------------------------
    void ensure_viewport_target(UVec2 size);
    void retire_viewport_targets(bool all);
    void update_editor_camera(f32 dt);
    void apply_camera_override();
    void restore_snapshot(const std::string& snapshot);
    // History wrappers: edits made while playing are not recorded (they vanish on Stop).
    void record_edit(const char* label);
    void begin_edit(const char* label);
    void end_edit();
    // Inspector / material panel: discrete commits and continuous ends are deferred to the end of
    // the panel so multi-edit propagation lands in the same undo step.
    void defer_commit(const char* label) { deferred_commit_ = label; }
    void defer_end() { deferred_end_ = true; }
    void flush_deferred_edits();
    Entity instantiate_model_at(const std::filesystem::path& rel, Entity parent, const std::optional<Vec3>& position);
    void   resolve_pick(const GpuPickTracker::Outcome& outcome);
    void   poll_gpu_pick();
    [[nodiscard]] gameplay::RenderResourceCache* render_cache();
    [[nodiscard]] std::filesystem::path          content_root() const;
    void                                         self_test_tick(); // self_test.cpp

    EditorOptions options_;
    EditHistory   history_;
    PlayState     play_state_   = PlayState::Edit;
    std::string   play_snapshot_;
    SelectionSet  selection_;
    std::filesystem::path scene_path_;

    // viewport render target
    struct ViewportTarget {
        rhi::TextureHandle texture;
        u64                imgui_id = 0;
        UVec2              size{ 0, 0 };
        u64                retire_frame = 0;
        bool               rendered = false; // left in ShaderRead by a previous frame
    };
    ViewportTarget              viewport_;
    std::vector<ViewportTarget> retired_;
    rhi::SamplerHandle          viewport_sampler_;
    Vec2                        viewport_min_{ 0.0f }; // screen-space image rect (last frame)
    Vec2                        viewport_size_{ 0.0f };
    bool                        viewport_hovered_ = false;
    bool                        viewport_focused_ = false;

    // editor camera
    gameplay::FlyCameraComponent fly_;
    Transform                    camera_{};
    gameplay::InputMap           camera_input_;
    bool                         looking_ = false;
    f32                          fov_y_deg_ = 60.0f;
    bool                         use_game_camera_ = true; // in play mode, when the scene has one

    // gizmo (group transform about the selection pivot)
    int          gizmo_operation_ = 0; // 0 translate, 1 rotate, 2 scale
    bool         gizmo_local_     = false;
    SnapSettings snap_;
    PivotMode    pivot_mode_      = PivotMode::BoundsCenter;
    bool         gizmo_dragging_  = false;
    Mat4         gizmo_matrix_{ 1.0f };
    Mat4         gizmo_start_pivot_{ 1.0f };
    std::vector<std::pair<u64, Mat4>> gizmo_start_worlds_; // selection roots at drag start

    // picking
    struct PendingPick {
        Ray        ray;
        SelectMode mode = SelectMode::Replace;
    };
    GpuPickTracker pick_tracker_;
    PendingPick    pending_pick_;
    std::string    last_pick_source_; // "gpu" / "cpu" / "cpu-fallback"
    bool           box_pending_ = false;
    Vec2           box_start_{ 0.0f };

    // materials: instance id -> registered data hash
    std::map<AssetId, u64> registered_materials_;
    i32                    material_slot_ = -1; // slot shown in the material editor
    std::string            deferred_commit_;
    bool                   deferred_end_ = false;

    // panels
    bool show_stats_     = true;
    bool show_console_   = true;
    bool show_assets_    = true;
    bool show_material_  = true;
    bool show_animation_ = true;
    bool dock_built_     = false;
    bool reset_layout_   = false; // View > Reset Layout: rebuild the default dock layout
    std::string rename_buffer_;
    u64         console_seen_ = 0;
    std::string console_filter_;
    bool        console_autoscroll_ = true;
    std::filesystem::path assets_dir_; // content-relative directory being browsed
    std::unique_ptr<ThumbnailCache> thumbnails_;
    std::unique_ptr<GraphEditor>    graph_editor_; // ADR-0018
    // UI scale (View > UI Scale): 0 = auto (follows the window height), else a fixed factor.
    // Persisted in <cache_dir>/editor_prefs.json. The style is rebuilt from base_style_ on change.
    bool                            grid_this_frame_ = false; // set by the viewport, drawn in on_render_frame
    f32                             ui_scale_setting_ = 0.0f;
    f32                             ui_scale_applied_ = 0.0f;
    std::unique_ptr<ImGuiStyle>     base_style_;
    void                            apply_ui_scale();
    void                            save_editor_prefs() const;
    void                            load_editor_prefs();
    bool                            show_graph_ = false;
    // projects window (ADR-0014)
    runtime::RecentProjects              recent_{ runtime::RecentProjects::default_file() };
    std::vector<runtime::ProjectEntry>   found_projects_;
    bool                                 show_projects_ = false;
    bool                                 projects_new_tab_ = false;
    std::string                          new_project_name_ = "My Project";
    std::string                          new_project_location_;
    int                                  new_project_template_ = 0; // 0 starter content, 1 empty
    std::string                          open_project_path_;
    std::string                          projects_error_;
    std::filesystem::path                pending_switch_; // waiting for the unsaved-changes answer
    Launcher                             launcher_;
    bool                            assets_grid_ = true;   // grid of thumbnails, else a list
    f32                             assets_tile_ = 88.0f;  // grid tile size (pixels)

    // file dialog
    enum class FileDialog : u8 { None = 0, Open, SaveAs, SavePrefab };
    FileDialog  file_dialog_ = FileDialog::None;
    std::string file_dialog_path_;

    // deferred structural edits from panels (applied after the panels are drawn)
    std::vector<std::pair<u64, u64>> pending_reparent_; // (child uuid, parent uuid or 0), one undo step
    bool                             pending_delete_selection_ = false;
    std::vector<std::pair<std::string, u64>> pending_asset_drops_; // (content path, parent uuid or 0)
    std::vector<std::pair<u64, bool>>        pending_prefab_ops_;  // (instance uuid, apply? else revert)

    // self-test
    u64  self_test_frame_  = 0;
    u32  self_test_step_   = 0;
    bool self_test_passed_ = true;
    std::vector<std::string> self_test_failures_;
    std::filesystem::path    self_test_dir_;
    std::filesystem::path    self_test_path_; // ADR-0018 graph file of the self-test
    f32  self_test_value_ = 0.0f;
    u64  self_test_uuid_  = 0;
    std::vector<u64> self_test_uuids_;
    usize self_test_count_ = 0;
};

} // namespace aether::editor
