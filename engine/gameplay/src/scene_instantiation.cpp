// scene_instantiation.cpp — SceneData -> entities (see scene_instantiation.h).
#include "aether/gameplay/scene_instantiation.h"

#include "aether/assets/asset_manager.h"
#include "aether/core/log.h"
#include "aether/gameplay/components.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/components.h"
#endif

#include <format>

namespace aether::gameplay {

Result<SceneInstance> instantiate_scene(World& world, const assets::SceneData& scene,
                                        const SceneInstantiateOptions& options) {
    if (options.parent != kNullEntity && !world.valid(options.parent)) {
        return Error{ ErrorCode::InvalidArgument, "instantiate_scene: invalid parent entity" };
    }
    for (usize i = 0; i < scene.nodes.size(); ++i) {
        const i32 p = scene.nodes[i].parent;
        if (p >= static_cast<i32>(i)) {
            return Error{ ErrorCode::InvalidArgument,
                          std::format("instantiate_scene: node {} has parent {} (parents must precede children)", i, p) };
        }
    }

    SceneInstance inst;
    const std::string root_name = !options.root_name.empty() ? options.root_name
                                  : !scene.name.empty()      ? scene.name
                                                             : std::string("Scene");
    inst.root = options.parent != kNullEntity ? world.create_child(options.parent, root_name) : world.create(root_name);
    inst.nodes.reserve(scene.nodes.size());

    for (usize i = 0; i < scene.nodes.size(); ++i) {
        const assets::SceneNodeData& node   = scene.nodes[i];
        const Entity                 parent = node.parent < 0 ? inst.root : inst.nodes[static_cast<usize>(node.parent)];
        const std::string            name   = node.name.empty() ? std::format("Node {}", i) : node.name;
        const Entity                 e      = world.create_child(parent, name);
        scene::set_local_transform(world, e, node.local);
        inst.nodes.push_back(e);

        if (node.mesh.is_valid()) {
            MeshRendererComponent mr;
            mr.mesh         = node.mesh;
            mr.material     = node.materials.empty() ? AssetId{} : node.materials.front();
            mr.cast_shadows = options.cast_shadows;
            world.add<MeshRendererComponent>(e, mr);
            if (node.materials.size() > 1) {
                world.add<MaterialOverridesComponent>(e, MaterialOverridesComponent{ node.materials });
            }
            ++inst.mesh_count;
        }
#if AE_WITH_ANIMATION
        if (options.add_animators && node.skeleton.is_valid()) {
            animation::AnimatorComponent anim;
            anim.skeleton_asset = node.skeleton;
            anim.graph_asset    = options.animation;
            world.add<animation::AnimatorComponent>(e, std::move(anim));
            ++inst.animator_count;
        }
#endif
    }
    world.update_transforms();
    return inst;
}

Result<SceneInstance> instantiate_model(assets::AssetManager& assets, World& world, StringView source_path,
                                        SceneInstantiateOptions options) {
    const auto scene = assets.load_sync<assets::SceneData>(source_path);
    if (!scene.ready()) {
        return Error{ ErrorCode::NotFound, std::format("instantiate_model('{}'): {}", source_path,
                                                       scene.error().message) };
    }
    if (!options.animation.is_valid() && options.auto_animation) {
        const AssetId clip = assets.resolve(source_path, "anim:0");
        if (clip.is_valid() && assets.database().find(clip).has_value()) {
            options.animation = clip;
        }
    }
    if (options.root_name.empty()) {
        const std::string path(source_path);
        const usize       slash = path.find_last_of('/');
        options.root_name       = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    return instantiate_scene(world, *scene.get(), options);
}

} // namespace aether::gameplay
