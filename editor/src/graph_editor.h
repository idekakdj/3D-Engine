// graph_editor.h — PRIVATE: the visual script (node graph) editor window (ADR-0018).
//
// Edits one .aegraph file at a time: a pannable / zoomable canvas (dragging empty space or
// right-dragging pans, the wheel zooms in fixed steps,
// right-click or dropping a wire on empty space opens the node menu), drag-to-wire between pins
// (type-checked; grabbing a connected data input picks its wire up, Alt+click clears a pin),
// inline literals for unconnected inputs, a variables list (the script's properties), a node
// palette, live compile errors (the failing nodes are outlined red) and the generated Lua. Undo /
// redo keep whole-graph snapshots. Saving writes the file; the ScriptVM hot reloads it like Lua.
// Main thread only; draw() must run inside an ImGui frame.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scripting/visual_script.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace aether::editor {

class GraphEditor {
public:
    // Opens `file` (absolute). `display` names it in the title (e.g. "scripts/door.aegraph").
    bool open(const std::filesystem::path& file, std::string display);
    // Creates `file` with the starter graph (fails if it exists) and opens it.
    bool create(const std::filesystem::path& file, std::string display);
    bool save();
    void close();

    // The editor window. `p_open` is the View-menu flag. `dock_id` (the Viewport's dock node, 0 =
    // none) receives the window as a tab the first time it is drawn in a session.
    void draw(bool* p_open, unsigned int dock_id = 0);

    [[nodiscard]] bool                         is_open() const noexcept { return !file_.empty(); }
    [[nodiscard]] bool                         dirty() const noexcept { return dirty_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    [[nodiscard]] scripting::VisualGraph&      graph() noexcept { return graph_; }
    [[nodiscard]] const scripting::GraphCompileResult& compiled();

    // Editing entry points shared by the UI and the self-test (each is one undo step).
    u32  add_node(std::string_view type, Vec2 canvas_pos);
    bool connect(u32 from, std::string_view from_pin, u32 to, std::string_view to_pin);
    void delete_selection();
    bool undo();
    bool redo();

private:
    struct PinRef {
        u32         node = 0;
        std::string pin;
        bool        output = false;
        explicit    operator bool() const noexcept { return node != 0; }
    };

    void snapshot(); // push the current graph onto the undo stack (before a change)
    void changed();  // after a change: dirty + recompile
    void draw_sidebar();
    void draw_canvas();
    void draw_status();
    void draw_node(u32 id, const Vec2& origin);
    void draw_create_menu();
    [[nodiscard]] Vec2   node_size(const scripting::GraphNode& n) const;
    [[nodiscard]] Vec2   pin_position(const scripting::GraphNode& n, std::string_view pin, bool output, const Vec2& origin) const;
    [[nodiscard]] PinRef pin_at(const Vec2& screen, const Vec2& origin) const;
    [[nodiscard]] u32    node_at(const Vec2& screen, const Vec2& origin) const;
    [[nodiscard]] Vec2   to_screen(const Vec2& canvas, const Vec2& origin) const { return origin + (canvas + scroll_) * zoom_; }
    [[nodiscard]] Vec2   to_canvas(const Vec2& screen, const Vec2& origin) const { return (screen - origin) / zoom_ - scroll_; }

    std::filesystem::path           file_;
    std::string                     display_;
    scripting::VisualGraph          graph_;
    scripting::GraphCompileResult   compiled_;
    bool                            compile_stale_ = true;
    bool                            dirty_ = false;
    std::vector<std::string>        undo_;
    std::vector<std::string>        redo_;
    std::set<u32>                   selection_;
    Vec2                            scroll_{ 40.0f, 40.0f };
    f32                             zoom_ = 1.0f;
    // Interaction state.
    PinRef                          drag_;            // wire being dragged
    bool                            moving_ = false;  // dragging selected nodes
    bool                            panning_ = false;      // right / middle drag
    bool                            panning_left_ = false; // left drag on empty canvas
    Vec2                            menu_canvas_pos_{ 0.0f };
    PinRef                          menu_connect_;    // wire dropped on empty space -> auto-connect
    bool                            open_menu_ = false;
    char                            search_[64] = {};
    bool                            show_lua_ = false;
    u32                             focus_node_ = 0; // scroll to this node next frame
    bool                            docked_once_ = false;
};

} // namespace aether::editor
