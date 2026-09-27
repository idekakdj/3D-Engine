// aether/editor/multi_edit.h — multi-entity property editing (Unreal-style multi-edit).
//
// The inspector draws the PRIMARY entity's component, remembers its value before the widgets
// ran, and afterwards copies only the fields that changed to every other selected entity that
// has the component: editing "Intensity" on three lights sets all three intensities but leaves
// their colours alone. The caller records the whole thing as one undo step (and patch()es
// signal-tracked components, see patch_all).
//
// Main thread only.
#pragma once

#include "aether/scene/entity.h"
#include "aether/scene/world.h"

#include <span>

namespace aether::editor {

// Copies each listed field that differs between `before` and `after` to every entity of `others`
// having T. Fields need operator==. Returns true if anything was written.
template <class T, class... M>
bool propagate_fields(World& world, const T& before, const T& after, std::span<const Entity> others, M T::*... fields) {
    bool any = false;
    auto one = [&](auto field) {
        if (before.*field == after.*field) {
            return;
        }
        for (const Entity o : others) {
            if (auto* c = world.try_get<T>(o)) {
                c->*field = after.*field;
                any       = true;
            }
        }
    };
    (one(fields), ...);
    return any;
}

// Emits the registry update signal for T on each entity that has it (physics / scripts react).
template <class T>
void patch_all(World& world, std::span<const Entity> entities) {
    for (const Entity e : entities) {
        if (world.has<T>(e)) {
            world.registry().template patch<T>(e);
        }
    }
}

} // namespace aether::editor
