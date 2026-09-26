// aether/editor/edit_history.h — snapshot-based undo/redo for the editor.
//
// Every undo step is a full scene snapshot (scene::save_scene_to_string). That is simple and
// exact (any edit - component fields, hierarchy, add/remove, script properties - is covered,
// including components owned by other modules through their codecs) and cheap enough for editor
// scenes. Protocol:
//   * mark_clean(world): the world's current state is the committed baseline (after load, undo,
//     redo or a finished edit).
//   * record(label, world): call AFTER a discrete edit (create, delete, reparent, add component):
//     pushes the baseline as the undo state and re-baselines.
//   * begin_continuous(label) ... end_continuous(world): for drags / text input spanning frames;
//     begin is idempotent within one gesture, so call it every frame the value changes.
//   * undo(world) / redo(world): restore through the `restore` callback (the editor reloads the
//     world with it, e.g. via Application::reload_world); return the restored label.
// Entity handles do not survive undo/redo (the world is reloaded); keep selections by uuid.
//
// Main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether {
class World;
}

namespace aether::editor {

class EditHistory {
public:
    using RestoreFn = std::function<Result<void>(World& world, const std::string& snapshot)>;

    explicit EditHistory(RestoreFn restore, usize max_steps = 128);

    void mark_clean(const World& world);
    void record(std::string label, const World& world);
    void begin_continuous(std::string label);
    void end_continuous(const World& world);
    [[nodiscard]] bool in_continuous() const noexcept { return continuous_; }

    // Returns the label of the undone/redone step, nullopt if nothing to do or restore failed.
    std::optional<std::string> undo(World& world);
    std::optional<std::string> redo(World& world);

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] usize undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] usize redo_count() const noexcept { return redo_.size(); }
    [[nodiscard]] const std::string& next_undo_label() const;
    [[nodiscard]] const std::string& next_redo_label() const;
    // Edits since the last save (dirty flag for the title bar / unsaved-changes prompts).
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void               mark_saved() noexcept { dirty_ = false; }

    void clear();

private:
    struct Step {
        std::string label;
        std::string snapshot;
    };
    void push_undo(Step step);

    RestoreFn         restore_;
    usize             max_steps_;
    std::vector<Step> undo_;
    std::vector<Step> redo_;
    std::string       baseline_;
    bool              continuous_ = false;
    bool              dirty_      = false;
};

} // namespace aether::editor
