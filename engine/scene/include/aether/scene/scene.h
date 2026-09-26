// aether/scene/scene.h — umbrella header for aether.scene + module-level documentation.
//
// WHAT LIVES WHERE
//   entity.h / components.h / world.h   frozen contracts (World, Entity, core components)
//   id.h                                IdComponent (persistent uuid) + find_by_uuid
//   transform_utils.h                   dirty-flag contract, set_local_*, world-space helpers,
//                                       set_parent_keep_world, update statistics
//   hierarchy_utils.h                   hierarchy model, traversal, name/path lookup, ordering
//   visibility.h                        hierarchical visibility
//   scene_serializer.h                  .aescene JSON, subtree copy/paste, clone_entity
//
// WORLD SEMANTICS (World is declared in the frozen world.h; behaviour documented here)
//   create(name)          adds Name + Transform + Hierarchy + Visibility + IdComponent; the
//                         entity is appended to the root list.
//   create_child(p, name) same, appended as p's last child (invalid p: warning, created as root).
//   destroy(e)            destroys e and its whole subtree, children first. Safe on invalid or
//                         already-destroyed entities (no-op).
//   set_parent(c, p)      O(1) relink; keeps c's LOCAL transform (use set_parent_keep_world to
//                         keep the world transform). p == kNullEntity detaches (c becomes the
//                         last root). Re-parenting to the current parent is a no-op (sibling
//                         order kept). Rejected with a warning and no change when c/p is
//                         invalid, p == c, or p is a descendant of c (cycle).
//   update_transforms()   recomputes world matrices of dirty subtrees only (see
//                         transform_utils.h). Iterative, allocation-free in steady state.
//   world_matrix(e)       always-correct world matrix (identity for invalid entities).
//   entity_count()        live entities (including ones created through the raw registry).
//   clear()               destroys everything; emits on_destroy for every component. Listeners
//                         must not mutate the hierarchy from inside those callbacks.
//   ~World()              does NOT emit on_destroy (EnTT semantics). Call clear() first if your
//                         listeners must observe teardown.
//
// COMPONENT LIFECYCLE HOOKS (for other modules)
//   Modules react to component creation/destruction through EnTT's registry signals rather
//   than through World callbacks:
//
//       struct PhysicsBridge {
//           void on_body_added(entt::registry& reg, entt::entity e);
//           void on_body_removed(entt::registry& reg, entt::entity e);
//       };
//       world.registry().on_construct<RigidBodyComponent>()
//            .connect<&PhysicsBridge::on_body_added>(bridge);
//       world.registry().on_destroy<RigidBodyComponent>()
//            .connect<&PhysicsBridge::on_body_removed>(bridge);
//       // ... and on shutdown, BEFORE `bridge` dies:
//       world.registry().on_construct<RigidBodyComponent>().disconnect(&bridge);
//       world.registry().on_destroy<RigidBodyComponent>().disconnect(&bridge);
//
//   * on_construct fires after the component is added (World::add on a new component,
//     clone_entity copies, scene loads). on_update fires on World::add over an existing
//     component (emplace_or_replace) and registry.patch/replace; direct writes through
//     get<T>() are silent. on_destroy fires before removal: World::remove, World::destroy
//     (children first), World::clear.
//   * Do not add/remove components on the entity being destroyed from inside on_destroy.
//   * The scene module itself listens to HierarchyComponent, IdComponent and
//     VisibilityComponent to keep the hierarchy lists, the uuid map and derived visibility
//     consistent; it stores all of its bookkeeping in registry.ctx(), so the frozen World
//     layout is unchanged.
//
// THREADING
//   The World is main-thread owned. Read-only queries may run on worker threads (e.g. render
//   extraction jobs) while no thread mutates the world.
#pragma once

#include "aether/scene/components.h"
#include "aether/scene/entity.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/id.h"
#include "aether/scene/scene_serializer.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"
