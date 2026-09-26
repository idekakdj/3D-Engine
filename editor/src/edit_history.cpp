// edit_history.cpp — snapshot undo/redo (see edit_history.h).
#include "aether/editor/edit_history.h"

#include "aether/core/log.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/world.h"

#include <utility>

namespace aether::editor {

namespace {
std::string snapshot_of(const World& world) {
    auto s = scene::save_scene_to_string(world);
    if (!s) {
        AE_LOG_ERROR("Editor", "undo snapshot failed: {}", s.error().message);
        return {};
    }
    return std::move(*s);
}
const std::string kEmpty;
} // namespace

EditHistory::EditHistory(RestoreFn restore, usize max_steps)
    : restore_(std::move(restore)), max_steps_(max_steps == 0 ? 1 : max_steps) {}

void EditHistory::mark_clean(const World& world) {
    baseline_   = snapshot_of(world);
    continuous_ = false;
}

void EditHistory::push_undo(Step step) {
    undo_.push_back(std::move(step));
    if (undo_.size() > max_steps_) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    dirty_ = true;
}

void EditHistory::record(std::string label, const World& world) {
    if (continuous_) {
        end_continuous(world); // a discrete edit ends any open gesture
    }
    push_undo(Step{ std::move(label), baseline_ });
    baseline_ = snapshot_of(world);
}

void EditHistory::begin_continuous(std::string label) {
    if (continuous_) {
        return;
    }
    continuous_ = true;
    push_undo(Step{ std::move(label), baseline_ });
}

void EditHistory::end_continuous(const World& world) {
    if (!continuous_) {
        return;
    }
    continuous_ = false;
    baseline_   = snapshot_of(world);
}

std::optional<std::string> EditHistory::undo(World& world) {
    if (continuous_) {
        end_continuous(world);
    }
    if (undo_.empty()) {
        return std::nullopt;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    const std::string current = snapshot_of(world);
    if (auto r = restore_(world, step.snapshot); !r) {
        AE_LOG_ERROR("Editor", "undo '{}' failed: {}", step.label, r.error().message);
        undo_.push_back(std::move(step));
        return std::nullopt;
    }
    redo_.push_back(Step{ step.label, current });
    baseline_ = std::move(step.snapshot);
    dirty_    = true;
    return step.label;
}

std::optional<std::string> EditHistory::redo(World& world) {
    if (continuous_) {
        end_continuous(world);
    }
    if (redo_.empty()) {
        return std::nullopt;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    const std::string current = snapshot_of(world);
    if (auto r = restore_(world, step.snapshot); !r) {
        AE_LOG_ERROR("Editor", "redo '{}' failed: {}", step.label, r.error().message);
        redo_.push_back(std::move(step));
        return std::nullopt;
    }
    undo_.push_back(Step{ step.label, current });
    baseline_ = std::move(step.snapshot);
    dirty_    = true;
    return step.label;
}

const std::string& EditHistory::next_undo_label() const { return undo_.empty() ? kEmpty : undo_.back().label; }
const std::string& EditHistory::next_redo_label() const { return redo_.empty() ? kEmpty : redo_.back().label; }

void EditHistory::clear() {
    undo_.clear();
    redo_.clear();
    continuous_ = false;
    dirty_      = false;
}

} // namespace aether::editor
