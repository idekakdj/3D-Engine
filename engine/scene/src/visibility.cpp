// visibility.cpp — derived VisibilityComponent::visible_in_hierarchy maintenance.
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/visibility.h"
#include "scene_internal.h"

namespace aether::scene::detail {
namespace {

// Derived flag that `e`'s children inherit (true when `e` has no VisibilityComponent).
bool inherited(const World& world, Entity e) {
    if (e == kNullEntity) {
        return true;
    }
    const VisibilityComponent* v = world.try_get<VisibilityComponent>(e);
    return v == nullptr || v->visible_in_hierarchy;
}

// Recomputes `e`'s derived flag from its parent. Returns true if it changed.
bool rederive(World& world, Entity e) {
    VisibilityComponent* v = world.try_get<VisibilityComponent>(e);
    if (v == nullptr) {
        return false; // children inherit "visible" through e regardless of e's ancestors
    }
    const bool value = v->visible && inherited(world, parent_of(world, e));
    if (value == v->visible_in_hierarchy) {
        return false;
    }
    v->visible_in_hierarchy = value;
    return true;
}

} // namespace

void refresh_visibility(World& world, Entity e) {
    if (!rederive(world, e)) {
        return; // subtree already consistent
    }
    for_each_descendant(world, e, [&](Entity d) {
        return rederive(world, d) ? Visit::Continue : Visit::SkipChildren;
    });
}

void on_visibility_construct(entt::registry& reg, Entity e) {
    SceneContext& ctx = context(reg);
    if (ctx.world != nullptr) {
        // A freshly added component may disagree with its parent (or with its existing
        // children's stored values); the default `visible_in_hierarchy = true` is re-derived.
        VisibilityComponent& v = reg.get<VisibilityComponent>(e);
        v.visible_in_hierarchy = !(v.visible && inherited(*ctx.world, parent_of(*ctx.world, e)));
        refresh_visibility(*ctx.world, e); // flips back to the correct value => propagates
    }
}

} // namespace aether::scene::detail

namespace aether::scene {

void set_visible(World& world, Entity e, bool visible) {
    if (!world.valid(e)) {
        return;
    }
    VisibilityComponent* v = world.try_get<VisibilityComponent>(e);
    if (v == nullptr) {
        // on_construct derives visible_in_hierarchy for the new component and its subtree.
        world.registry().emplace<VisibilityComponent>(e, VisibilityComponent{ visible, true });
        return;
    }
    v->visible = visible;
    detail::refresh_visibility(world, e);
}

bool is_visible(const World& world, Entity e) {
    const VisibilityComponent* v = world.try_get<VisibilityComponent>(e);
    return v == nullptr || v->visible;
}

bool is_visible_in_hierarchy(const World& world, Entity e) {
    const VisibilityComponent* v = world.try_get<VisibilityComponent>(e);
    return v == nullptr || v->visible_in_hierarchy;
}

void update_visibility(World& world) {
    auto derive = [&](Entity e) {
        if (VisibilityComponent* v = world.try_get<VisibilityComponent>(e)) {
            v->visible_in_hierarchy =
                v->visible && detail::inherited(world, parent_of(world, e));
        }
    };
    // Pre-order guarantees parents are derived before their children.
    for_each_in_hierarchy(world, derive);
    // Entities outside the hierarchy have no parent.
    for (auto [e, v] : world.registry().storage<VisibilityComponent>().each()) {
        if (!world.has<HierarchyComponent>(e)) {
            v.visible_in_hierarchy = v.visible;
        }
    }
}

} // namespace aether::scene
