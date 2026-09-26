// aether/scene/world.h — the ECS world + transform hierarchy.
// FROZEN CONTRACT (ADR-0001). Wraps an entt::registry with ergonomic helpers.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/components.h"
#include "aether/scene/entity.h"

#include <string>
#include <utility>

namespace aether {

class World {
public:
    World();
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- entity lifecycle ----
    Entity create(std::string name = "Entity"); // adds Name, Transform, Hierarchy, Visibility, Id
    Entity create_child(Entity parent, std::string name = "Entity");
    void   destroy(Entity e);                                    // recursively destroys children
    [[nodiscard]] bool valid(Entity e) const;

    // ---- components (templated passthrough; EnTT in the public API per amendment) ----
    template <typename T, typename... Args>
    T& add(Entity e, Args&&... args) {
        return registry_.emplace_or_replace<T>(e, std::forward<Args>(args)...);
    }
    template <typename T>       T*  try_get(Entity e)       { return registry_.try_get<T>(e); }
    template <typename T> const T*  try_get(Entity e) const { return registry_.try_get<T>(e); }
    template <typename T>       T&  get(Entity e)           { return registry_.get<T>(e); }
    template <typename T> bool      has(Entity e) const     { return registry_.all_of<T>(e); }
    template <typename T> void      remove(Entity e)        { registry_.remove<T>(e); }

    template <typename... T> auto view() { return registry_.view<T...>(); }

    entt::registry&       registry()       { return registry_; }
    const entt::registry& registry() const { return registry_; }

    // ---- hierarchy ----
    void set_parent(Entity child, Entity parent); // parent == kNullEntity detaches
    // Recompute cached world matrices for dirty transforms in hierarchy order.
    void update_transforms();

    [[nodiscard]] Mat4 world_matrix(Entity e) const;

    [[nodiscard]] usize entity_count() const;
    void clear();

private:
    entt::registry registry_;
};

} // namespace aether
