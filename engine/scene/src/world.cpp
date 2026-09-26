// world.cpp — World lifecycle, entity creation/destruction and set_parent.
// (update_transforms / world_matrix live in transform.cpp; semantics: aether/scene/scene.h.)
#include "aether/core/log.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/world.h"
#include "scene_internal.h"

#include <algorithm>
#include <vector>

namespace aether {

World::World() {
    auto& ctx = registry_.ctx().emplace<scene::detail::SceneContext>();
    ctx.world = this;
    scene::detail::seed_uuid_generator(ctx);
    scene::detail::connect_listeners(registry_);
}

// EnTT semantics: destroying the registry does not emit on_destroy (see scene.h).
World::~World() = default;

Entity World::create(std::string name) {
    return scene::detail::create_entity(*this, std::move(name), kNullEntity, 0);
}

Entity World::create_child(Entity parent, std::string name) {
    if (parent != kNullEntity && !registry_.valid(parent)) {
        AE_LOG_WARN(scene::detail::kLogCategory,
                    "create_child: invalid parent {}; '{}' created as a root",
                    scene::detail::to_string(parent), name);
        parent = kNullEntity;
    }
    return scene::detail::create_entity(*this, std::move(name), parent, 0);
}

void World::destroy(Entity e) {
    if (!registry_.valid(e)) {
        return;
    }
    const HierarchyComponent* h = registry_.try_get<HierarchyComponent>(e);
    if (h == nullptr || h->first_child == kNullEntity) {
        registry_.destroy(e);
        return;
    }
    // Pre-order snapshot, destroyed in reverse: every entity goes after all its descendants,
    // so each on_destroy sees a leaf and unlinks in O(1).
    std::vector<Entity> subtree;
    subtree.push_back(e);
    scene::for_each_descendant(*this, e, [&](Entity d) { subtree.push_back(d); });
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        if (registry_.valid(*it)) { // a listener may already have destroyed it
            registry_.destroy(*it);
        }
    }
}

bool World::valid(Entity e) const {
    return registry_.valid(e);
}

void World::set_parent(Entity child, Entity parent) {
    scene::detail::reparent(*this, child, parent, kNullEntity, true);
}

usize World::entity_count() const {
    return registry_.storage<Entity>()->free_list();
}

void World::clear() {
    scene::detail::SceneContext& ctx = scene::detail::context(registry_);
    ctx.bulk_clear = true;
    registry_.clear();
    ctx.bulk_clear = false;

    ctx.first_root = kNullEntity;
    ctx.last_root  = kNullEntity;
    ctx.root_count = 0;
    std::fill(ctx.last_child.begin(), ctx.last_child.end(), Entity{ kNullEntity });
    ctx.uuid_to_entity.clear();
    ctx.stats = {};
}

} // namespace aether
