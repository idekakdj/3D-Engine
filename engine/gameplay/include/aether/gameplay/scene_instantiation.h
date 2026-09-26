// aether/gameplay/scene_instantiation.h — turning imported scene assets into entities.
//
// instantiate_scene() creates one entity per assets::SceneNodeData under a new root entity:
//   * name + local transform from the node;
//   * MeshRendererComponent (mesh, first material) for nodes with a mesh, plus a
//     MaterialOverridesComponent when the node has several material slots;
//   * an animation::AnimatorComponent (skeleton_asset + optional clip in graph_asset) for skinned
//     nodes, when the animation module is part of the build. Gameplay's AnimationAssetBinder
//     resolves those ids into runtime objects (animation_bridge.h).
// instantiate_model() loads a source file's primary scene through the AssetManager first.
//
// Main thread only.
#pragma once

#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <string>
#include <vector>

namespace aether {
class World;
}
namespace aether::assets {
class AssetManager;
struct SceneData;
} // namespace aether::assets

namespace aether::gameplay {

struct SceneInstantiateOptions {
    Entity      parent = kNullEntity; // the new root is appended here (kNullEntity: as a root)
    std::string root_name;            // empty: the scene's name (or "Scene")
    bool        cast_shadows = true;
    // Skinned nodes: add an AnimatorComponent (needs AE_WITH_ANIMATION). `animation` is the clip
    // to play (an AnimationClip AssetId); instantiate_model() picks the source's first clip when
    // it is left invalid and `auto_animation` is set.
    bool    add_animators  = true;
    AssetId animation{};
    bool    auto_animation = true;
};

struct SceneInstance {
    Entity              root = kNullEntity;
    std::vector<Entity> nodes; // parallel to SceneData::nodes
    u32                 mesh_count     = 0;
    u32                 animator_count = 0;
};

[[nodiscard]] Result<SceneInstance> instantiate_scene(World& world, const assets::SceneData& scene,
                                                      const SceneInstantiateOptions& options = {});

// Loads `source_path` (content-relative, e.g. "samples/cube/cube.gltf") synchronously and
// instantiates its primary scene.
[[nodiscard]] Result<SceneInstance> instantiate_model(assets::AssetManager& assets, World& world,
                                                      StringView source_path,
                                                      SceneInstantiateOptions options = {});

} // namespace aether::gameplay
