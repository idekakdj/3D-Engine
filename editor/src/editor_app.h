// editor_app.h — PRIVATE: the Aether editor application (Layer 5).
//
// A gameplay::Application whose frame renders the world into an offscreen viewport texture
// shown in a docked ImGui window, surrounded by the hierarchy, inspector, asset browser and
// console panels. Editing happens with the simulation paused; Play snapshots the world and runs
// it, Stop restores the snapshot (play-in-editor). Every edit goes through EditHistory
// (snapshot undo/redo). Selection is kept by entity uuid so it survives undo and reloads.
//
// The panel implementations live in panel_*.cpp; actions (create/delete/save/play...) in
// editor_app.cpp; the scripted --self-test in self_test.cpp.
#pragma once

#include "aether/editor/edit_history.h"
#include "aether/editor/picking.h"
#include "aether/gameplay/application.h"
#include "aether/gameplay/camera_controller.h"
#include "aether/gameplay/input_map.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/renderer/render_scene.h"
#include "aether/rhi/resources.h"
#include "aether/scene/entity.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aether::gameplay {
class RenderResourceCache;
}

namespace aether::editor {

struct EditorOptions {
    bool        self_test = false; // run the scripted self-test and exit with its result
    std::string select;            // entity name to select (and focus) after startup
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
    void   undo();
    void   redo();
    void   play();
    void   stop();
    void   toggle_pause();
    void   step();
    void   focus(Entity e);

    void                   select(Entity e);
    [[nodiscard]] Entity   selected();
    [[nodiscard]] PlayState play_state() const noexcept { return play_state_; }
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
    [[nodiscard]] gameplay::RenderResourceCache* render_cache();
    [[nodiscard]] std::filesystem::path          content_root() const;
    void                                         self_test_tick(); // self_test.cpp

    EditorOptions options_;
    EditHistory   history_;
    PlayState     play_state_   = PlayState::Edit;
    std::string   play_snapshot_;
    u64           selected_uuid_ = 0;
    std::filesystem::path scene_path_;

    // viewport render target
    struct ViewportTarget {
        rhi::TextureHandle texture;
        u64                imgui_id = 0;
        UVec2              size{ 0, 0 };
        u64                retire_frame = 0;
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

    // gizmo
    int  gizmo_operation_ = 0; // 0 translate, 1 rotate, 2 scale
    bool gizmo_local_     = false;
    bool gizmo_snap_      = false;
    f32  snap_translate_  = 0.5f;
    f32  snap_rotate_deg_ = 15.0f;
    f32  snap_scale_      = 0.1f;
    bool gizmo_was_using_ = false;

    // panels
    bool show_stats_     = true;
    bool show_console_   = true;
    bool show_assets_    = true;
    bool dock_built_     = false;
    std::string rename_buffer_;
    u64         console_seen_ = 0;
    std::string console_filter_;
    bool        console_autoscroll_ = true;
    std::filesystem::path assets_dir_; // content-relative directory being browsed

    // file dialog
    enum class FileDialog : u8 { None = 0, Open, SaveAs };
    FileDialog  file_dialog_ = FileDialog::None;
    std::string file_dialog_path_;

    // deferred structural edits from panels (applied after the panels are drawn)
    std::vector<std::pair<u64, u64>> pending_reparent_; // (child uuid, parent uuid or 0)
    std::vector<u64>                 pending_delete_;

    // self-test
    u64  self_test_frame_  = 0;
    u32  self_test_step_   = 0;
    bool self_test_passed_ = true;
    std::vector<std::string> self_test_failures_;
    std::filesystem::path    self_test_dir_;
    f32  self_test_value_ = 0.0f;
    u64  self_test_uuid_  = 0;
};

} // namespace aether::editor
