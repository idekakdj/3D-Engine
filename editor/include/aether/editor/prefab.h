// aether/editor/prefab.h — prefab assets (.aeprefab) and editor-side scene codecs.
//
// A prefab file is a scene-serializer subtree document ("kind": "entities", see
// scene_serializer.h) holding one or more top-level entities with their whole subtrees. The
// top-level transforms are stored RELATIVE TO A PIVOT (the selection pivot when the prefab was
// created), so instantiating with a placement matrix P puts each top-level entity at
// P * stored_local. Every component with a codec round-trips (physics, scripts, animators,
// material overrides, the editor components below).
//
// Instances remember their source: each instantiated top-level entity gets a
// PrefabInstanceComponent. It is an EDITOR component serialized by the "EditorPrefabInstance"
// codec that register_editor_codecs() installs on the editor's World (scenes saved by the editor
// carry the link; other loaders skip the unknown component with a warning). Nested prefab links
// inside a prefab are kept; the top-level links are stripped when a prefab is written.
//
// Main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace aether {
class World;
}

namespace aether::editor {

inline constexpr StringView kPrefabExtension      = ".aeprefab";
inline constexpr StringView kPrefabCodecName      = "EditorPrefabInstance";

struct PrefabInstanceComponent {
    std::string source;         // content-relative generic path (absolute when outside the content root)
    u32         root_index = 0; // which top-level entity of the prefab this instance root is
};

// Registers the editor codecs (prefab links + material instances, material_instance.h) on
// `world`. Codecs live in the registry context, so they survive World::clear / reload_world.
bool register_editor_codecs(World& world);

// Serializes `roots` (and their subtrees) as a prefab document; top-level transforms are written
// as inverse(pivot) * world. Roots that descend from another listed root are folded into it.
[[nodiscard]] Result<std::string> make_prefab_document(const World& world, std::span<const Entity> roots,
                                                       const Mat4& pivot);
[[nodiscard]] Result<void> write_prefab(const World& world, std::span<const Entity> roots, const Mat4& pivot,
                                        const std::filesystem::path& file);
[[nodiscard]] Result<std::string> read_text_file(const std::filesystem::path& file);
// Number of top-level entities in a prefab / subtree document (0 if it is not one).
[[nodiscard]] usize prefab_root_count(StringView json);

// Instantiates a prefab (or any subtree/scene document) with fresh uuids. Top-level entities are
// appended under `parent` (kNullEntity = roots) with local transform placement * stored; when
// `source` is not empty each gets PrefabInstanceComponent{source, index of the top-level entity}.
[[nodiscard]] Result<std::vector<Entity>> instantiate_prefab_document(World& world, StringView json,
                                                                      StringView source, Entity parent,
                                                                      const Mat4& placement);
[[nodiscard]] Result<std::vector<Entity>> instantiate_prefab(World& world, const std::filesystem::path& file,
                                                             StringView source, Entity parent,
                                                             const Mat4& placement);

// Apply: overwrites `file` with the instance's subtree (the instance root becomes the prefab's
// identity root); refused for multi-root prefabs (it would drop the other roots). Revert:
// replaces the instance with a fresh copy of its top-level entity (root_index) from `file`,
// keeping the instance's parent, sibling position, local transform and name.
[[nodiscard]] Result<void>   apply_prefab_instance(const World& world, Entity instance_root,
                                                   const std::filesystem::path& file);
[[nodiscard]] Result<Entity> revert_prefab_instance(World& world, Entity instance_root,
                                                    const std::filesystem::path& file);

} // namespace aether::editor
