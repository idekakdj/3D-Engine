// aether/scene/scene_serializer.h — JSON scene files (.aescene), subtree copy/paste,
// prefab-style subtree files, and in-world entity cloning.
//
// FILE FORMAT (version 1)
// -----------------------
//   {
//     "format": "aether.scene",
//     "version": 1,
//     "kind": "scene" | "entities",          // informational: full world vs. copied subtrees
//     "entities": [                          // depth-first pre-order: parents precede children
//       {
//         "uuid": "9f3a0c1d2e4b5a67",        // 16 hex digits (IdComponent)
//         "parent": "0123456789abcdef",      // omitted for roots
//         "components": {
//           "Name":         { "name": "Arm" },
//           "Transform":    { "position": [x,y,z], "rotation": [x,y,z,w], "scale": [x,y,z] },
//           "Visibility":   { "visible": true },
//           "Tag":          { "tag": 0 },
//           "MeshRenderer": { "mesh": "<32 hex>", "material": "<32 hex>", "cast_shadows": true,
//                             "local_bounds": { "min": [x,y,z], "max": [x,y,z] } },
//           "Light":        { "kind": "Directional"|"Point"|"Spot", "color": [r,g,b],
//                             "intensity": 1, "range": 10, "inner_cone_deg": 30,
//                             "outer_cone_deg": 35, "cast_shadows": false },
//           "Camera":       { "fov_y_deg": 60, "near_z": 0.1, "far_z": 1000, "primary": true }
//         }
//       }
//     ]
//   }
// * Hierarchy is stored as parent uuids; sibling order = array order.
// * AssetIds are 32 lowercase hex digits (hi then lo, same as AssetId::to_string()).
// * Floats are written in their shortest exact form; save -> load reproduces every value
//   bit-for-bit. Derived data (TransformComponent::world/dirty,
//   VisibilityComponent::visible_in_hierarchy, HierarchyComponent links) is not stored; it
//   is rebuilt on load (load ends with World::update_transforms()).
// * Loading is lenient where it is safe: missing fields keep their defaults, unknown
//   components / malformed fields / dangling parents log a warning and are skipped (counted
//   in SceneLoadResult::warning_count). Structural errors (not JSON, wrong "format",
//   unsupported "version", no "entities" array) fail with an Error and — for load_scene with
//   clear_world — leave the world untouched (the document is validated before clearing).
// * Every loaded entity gets the World::create() component set (Name, Transform, Hierarchy,
//   Visibility, Id); other components only if present in the file.
// * Built in: the frozen scene components + IdComponent. Components owned by other modules
//   are serialized through ComponentCodecs registered on the World (see below) until the
//   reflection backend (blueprint §5.4) makes this automatic. Unregistered components are
//   not saved.
//
// Thread-affinity: all functions are main-thread only (save functions only read the world
// and may run on another thread if nothing mutates the world concurrently).
#pragma once

#include "aether/core/error.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace aether {
class World;
} // namespace aether

namespace aether::scene {

inline constexpr u32        kSceneFormatVersion = 1;
inline constexpr StringView kSceneFormatName    = "aether.scene";
inline constexpr StringView kSceneFileExtension = ".aescene";

struct SceneLoadOptions {
    // true: World::clear() first (after the document validated), i.e. "open scene".
    // false: additive — loaded entities are appended to the existing world.
    bool clear_world = true;
    // true: entities keep the uuids stored in the file (a uuid that collides with an entity
    // already in the world is replaced by a fresh one, with a warning).
    // false: every loaded entity gets a fresh uuid (instantiating the same file twice).
    bool keep_uuids = true;
};

struct SceneLoadResult {
    std::vector<Entity> roots;             // top-level entities created, in file order
    usize               entity_count  = 0; // entities created
    usize               warning_count = 0; // recoverable problems (each was logged)
};

// ---- extension point: components owned by other modules ------------------------------------------
// A codec (de)serializes one component type under "components"/<name>. JSON crosses the
// boundary as TEXT so no JSON library leaks into public headers. Codecs are registered per
// World (stored in its registry context, so they survive World::clear() / reload_world) —
// typically once at subsystem startup.
//   * save: write the component of `e` as one JSON value (object, array or scalar) into
//     `out_json` and return true; return false if `e` has no such component. Invalid JSON is
//     logged as an error and the component is omitted (the rest of the scene still saves).
//   * load: apply a previously saved value. Runs after every entity of the document exists and
//     the hierarchy is linked, so find_by_uuid() resolves references inside the document
//     (when loading with fresh uuids — paste / keep_uuids=false — stored uuids refer to the
//     ORIGINAL entities; remapping is the codec's responsibility). Returning an Error makes the
//     loader warn and skip the component.
struct ComponentCodec {
    std::string name;
    std::function<bool(const World& world, Entity e, std::string& out_json)>  save;
    std::function<Result<void>(World& world, Entity e, StringView json)>     load;
};

// Registers `codec`, replacing a codec of the same name. Fails (logs) for an empty name,
// missing functions, or a built-in component name. Main thread only.
bool register_component_codec(World& world, ComponentCodec codec);
// Removes the codec named `name` (no-op if absent). Main thread only.
void unregister_component_codec(World& world, StringView name);

// ---- whole worlds ---------------------------------------------------------------------------
// Serializes every entity: the hierarchy in pre-order (roots in root order), followed by any
// entity without a HierarchyComponent (as a root).
[[nodiscard]] Result<std::string> save_scene_to_string(const World& world);
// Writes atomically (temp file + rename); creates missing parent directories.
[[nodiscard]] Result<void> save_scene(const World& world, const std::filesystem::path& file);

[[nodiscard]] Result<SceneLoadResult> load_scene_from_string(World& world, StringView json,
                                                             const SceneLoadOptions& options = {});
[[nodiscard]] Result<SceneLoadResult> load_scene(World& world, const std::filesystem::path& file,
                                                 const SceneLoadOptions& options = {});

// ---- subtrees: copy/paste and prefab-style files ------------------------------------------------
// Serializes each root's subtree ("kind": "entities"). Roots that are descendants of another
// listed root are folded into it; invalid entities are skipped. Roots are written without a
// parent reference. Fails if no valid root was given.
[[nodiscard]] Result<std::string> save_entities_to_string(const World& world,
                                                          std::span<const Entity> roots);
[[nodiscard]] Result<std::string> save_subtree_to_string(const World& world, Entity root);
[[nodiscard]] Result<void> save_subtree(const World& world, Entity root,
                                        const std::filesystem::path& file);

// Instantiates a document additively with FRESH uuids (parent references inside the document
// are remapped). Top-level entities are appended under `parent` (kNullEntity = as roots),
// keeping their local transforms. Returns the new top-level entities in document order.
[[nodiscard]] Result<std::vector<Entity>> load_entities_from_string(World& world, StringView json,
                                                                    Entity parent = kNullEntity);
// As above, returning the first top-level entity (fails if the document has none).
[[nodiscard]] Result<Entity> load_subtree_from_string(World& world, StringView json,
                                                      Entity parent = kNullEntity);
[[nodiscard]] Result<Entity> load_subtree(World& world, const std::filesystem::path& file,
                                          Entity parent = kNullEntity);

// ---- in-world duplication ---------------------------------------------------------------------
// Deep-copies `source` and its whole subtree inside the same world (editor "Duplicate",
// prefab instancing). EVERY copyable component is copied — including components owned by other
// modules (on_construct listeners fire for the copies, so modules can re-create runtime state) —
// except HierarchyComponent (rebuilt: same structure and sibling order) and IdComponent (fresh
// uuids). Entity-valued fields inside copied components are copied verbatim (not remapped).
// The copy is inserted right after `source` among its siblings. Returns the copy's root, or
// kNullEntity if `source` is invalid. Main thread only.
[[nodiscard]] Entity clone_entity(World& world, Entity source);
// Same, but the copy is appended under `new_parent` (kNullEntity = as the last root).
[[nodiscard]] Entity clone_entity(World& world, Entity source, Entity new_parent);

} // namespace aether::scene
