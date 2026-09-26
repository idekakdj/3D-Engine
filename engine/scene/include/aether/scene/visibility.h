// aether/scene/visibility.h — hierarchical visibility.
//
// VisibilityComponent::visible is the entity's own flag; `visible_in_hierarchy` is derived:
//     visible_in_hierarchy(e) = visible(e) && visible_in_hierarchy(parent(e))
// (a missing parent, or a parent without a VisibilityComponent, counts as visible).
//
// The World keeps `visible_in_hierarchy` up to date EAGERLY for every structural change it
// performs (create, create_child, set_parent, destroy, adding a VisibilityComponent). Updates
// propagate down the affected subtree and stop at the first entity whose derived value does
// not change, so reparenting is O(1) unless visibility actually flips.
//
// Toggle visibility with set_visible() (keeps the derived flags consistent). If you write
// VisibilityComponent::visible directly, call update_visibility() afterwards.
//
// Thread-affinity: mutators are main-thread only; queries are safe for concurrent readers
// while no thread mutates the world.
#pragma once

#include "aether/scene/entity.h"

namespace aether {
class World;
} // namespace aether

namespace aether::scene {

// Sets the entity's own flag (adding a VisibilityComponent if missing) and refreshes the
// derived flag of its subtree.
void set_visible(World& world, Entity e, bool visible);

// Own flag (true if the entity has no VisibilityComponent).
[[nodiscard]] bool is_visible(const World& world, Entity e);
// Derived flag (true if the entity has no VisibilityComponent). O(1).
[[nodiscard]] bool is_visible_in_hierarchy(const World& world, Entity e);

// Recomputes `visible_in_hierarchy` for every entity. O(n); needed only after writing
// VisibilityComponent::visible directly.
void update_visibility(World& world);

} // namespace aether::scene
