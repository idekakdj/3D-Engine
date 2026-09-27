// aether/editor/selection.h — the editor's multi-selection set.
//
// Entities are kept by uuid (IdComponent), so a selection survives undo/redo and world reloads
// (entity handles do not). The set is ordered by selection time; the PRIMARY (active) entity is
// the most recently selected one, as in Unreal: the inspector shows its values, and "local"
// gizmo orientation uses its rotation.
//
// Click semantics (viewport and hierarchy):
//   plain click  -> select only the clicked entity (clicking empty space clears)
//   Ctrl+click   -> toggle the clicked entity (empty space: no change)
//   Shift+click  -> add the clicked entity (empty space: no change); re-adding makes it primary
//
// selection_roots() reduces a set of entities to the ones without a selected ancestor: group
// transforms, duplicate, delete, reparent and "create prefab" operate on those roots only, so a
// parent and its child are never moved twice.
//
// Main thread only.
#pragma once

#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <span>
#include <vector>

namespace aether {
class World;
}

namespace aether::editor {

enum class SelectMode : u8 {
    Replace = 0, // plain click
    Toggle,      // Ctrl
    Add,         // Shift
};

// Ctrl wins over Shift (matches the hierarchy panel and the viewport).
[[nodiscard]] constexpr SelectMode select_mode_from(bool ctrl, bool shift) noexcept {
    return ctrl ? SelectMode::Toggle : shift ? SelectMode::Add : SelectMode::Replace;
}

class SelectionSet {
public:
    void clear() noexcept { uuids_.clear(); }
    void set(u64 uuid);              // exactly {uuid} (0 clears)
    void add(u64 uuid);              // appends, or moves an existing uuid to primary
    bool remove(u64 uuid);           // true if it was selected
    void toggle(u64 uuid);           // add if absent, remove if present
    void set_all(std::span<const u64> uuids); // replaces; last = primary; zeros and duplicates dropped

    // One click on `uuid` (0 = empty space) with the given modifier semantics (see file comment).
    void apply_click(u64 uuid, SelectMode mode);

    [[nodiscard]] bool  contains(u64 uuid) const noexcept;
    [[nodiscard]] u64   primary() const noexcept { return uuids_.empty() ? 0 : uuids_.back(); }
    [[nodiscard]] usize size() const noexcept { return uuids_.size(); }
    [[nodiscard]] bool  empty() const noexcept { return uuids_.empty(); }
    [[nodiscard]] const std::vector<u64>& uuids() const noexcept { return uuids_; } // oldest first

    // Drops uuids that no longer resolve in `world`; returns how many were dropped.
    usize prune(const World& world);
    // The valid entities, oldest first (primary last).
    [[nodiscard]] std::vector<Entity> entities(const World& world) const;

private:
    std::vector<u64> uuids_;
};

// Entities of `selection` that have no strict ancestor in `selection` (order preserved; invalid
// entities and duplicates dropped).
[[nodiscard]] std::vector<Entity> selection_roots(const World& world, std::span<const Entity> selection);

} // namespace aether::editor
