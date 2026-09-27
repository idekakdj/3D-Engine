// selection.cpp — see selection.h.
#include "aether/editor/selection.h"

#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/world.h"

#include <algorithm>

namespace aether::editor {

void SelectionSet::set(u64 uuid) {
    uuids_.clear();
    if (uuid != 0) {
        uuids_.push_back(uuid);
    }
}

void SelectionSet::add(u64 uuid) {
    if (uuid == 0) {
        return;
    }
    std::erase(uuids_, uuid);
    uuids_.push_back(uuid);
}

bool SelectionSet::remove(u64 uuid) { return std::erase(uuids_, uuid) > 0; }

void SelectionSet::toggle(u64 uuid) {
    if (uuid == 0) {
        return;
    }
    if (!remove(uuid)) {
        uuids_.push_back(uuid);
    }
}

void SelectionSet::set_all(std::span<const u64> uuids) {
    uuids_.clear();
    for (const u64 u : uuids) {
        add(u);
    }
}

void SelectionSet::apply_click(u64 uuid, SelectMode mode) {
    switch (mode) {
    case SelectMode::Replace: set(uuid); break;
    case SelectMode::Toggle: toggle(uuid); break;
    case SelectMode::Add: add(uuid); break;
    }
}

bool SelectionSet::contains(u64 uuid) const noexcept {
    return uuid != 0 && std::find(uuids_.begin(), uuids_.end(), uuid) != uuids_.end();
}

usize SelectionSet::prune(const World& world) {
    return std::erase_if(uuids_, [&](u64 u) { return scene::find_by_uuid(world, u) == kNullEntity; });
}

std::vector<Entity> SelectionSet::entities(const World& world) const {
    std::vector<Entity> out;
    out.reserve(uuids_.size());
    for (const u64 u : uuids_) {
        const Entity e = scene::find_by_uuid(world, u);
        if (e != kNullEntity) {
            out.push_back(e);
        }
    }
    return out;
}

std::vector<Entity> selection_roots(const World& world, std::span<const Entity> selection) {
    std::vector<Entity> out;
    for (const Entity e : selection) {
        if (!world.valid(e) || std::find(out.begin(), out.end(), e) != out.end()) {
            continue;
        }
        const bool has_selected_ancestor = std::any_of(selection.begin(), selection.end(), [&](Entity other) {
            return other != e && world.valid(other) && scene::is_ancestor(world, other, e);
        });
        if (!has_selected_ancestor) {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace aether::editor
