// test_scene_instantiation.cpp — SceneData -> entities, and loading models through assets.
#include "gameplay_test_utils.h"

#include "aether/gameplay/animation_bridge.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/scene_instantiation.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/animation_subsystem.h"
#    include "aether/animation/components.h"
#endif

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::gameplay;
using namespace aether::gameplay::test;

TEST_CASE("instantiate_scene builds the node hierarchy and components") {
    assets::SceneData scene;
    scene.name = "Level";
    assets::SceneNodeData root;
    root.name           = "Root";
    root.local.position = Vec3(1, 0, 0);
    assets::SceneNodeData child;
    child.name           = "Child";
    child.parent         = 0;
    child.local.position = Vec3(0, 2, 0);
    child.mesh           = AssetId{ 1, 1 };
    child.materials      = { AssetId{ 2, 2 }, AssetId{ 3, 3 } };
    assets::SceneNodeData single;
    single.mesh      = AssetId{ 4, 4 };
    single.materials = { AssetId{ 5, 5 } };
    scene.nodes      = { root, child, single };

    World        world;
    const Entity holder = world.create("Holder");
    SceneInstantiateOptions opt;
    opt.parent       = holder;
    opt.cast_shadows = false;
    auto inst = instantiate_scene(world, scene, opt);
    REQUIRE(inst.has_value());
    CHECK(inst->mesh_count == 2);
    REQUIRE(inst->nodes.size() == 3);
    CHECK((scene::parent_of(world, inst->root) == holder));
    CHECK(world.get<NameComponent>(inst->root).name == "Level");
    CHECK((scene::parent_of(world, inst->nodes[1]) == inst->nodes[0]));
    CHECK((scene::parent_of(world, inst->nodes[2]) == inst->root));
    CHECK(world.get<NameComponent>(inst->nodes[2]).name == "Node 2"); // unnamed nodes get a name
    CHECK(scene::world_position(world, inst->nodes[1]) == Vec3(1, 2, 0));

    const auto& mr = world.get<MeshRendererComponent>(inst->nodes[1]);
    CHECK(mr.mesh == AssetId{ 1, 1 });
    CHECK(mr.material == AssetId{ 2, 2 });
    CHECK_FALSE(mr.cast_shadows);
    REQUIRE(world.has<MaterialOverridesComponent>(inst->nodes[1]));
    CHECK(world.get<MaterialOverridesComponent>(inst->nodes[1]).materials.size() == 2);
    CHECK_FALSE(world.has<MaterialOverridesComponent>(inst->nodes[2]));
    CHECK_FALSE(world.has<MeshRendererComponent>(inst->nodes[0]));

    // Invalid inputs.
    assets::SceneData bad = scene;
    bad.nodes[0].parent   = 2;
    CHECK_FALSE(instantiate_scene(world, bad).has_value());
    SceneInstantiateOptions dangling;
    dangling.parent = Entity{ 12345 };
    CHECK_FALSE(instantiate_scene(world, scene, dangling).has_value());
}

TEST_CASE("instantiate_model loads the cube sample") {
    AssetFixture assets;
    World        world;
    auto         inst = instantiate_model(assets.manager, world, "samples/cube/cube.gltf");
    REQUIRE(inst.has_value());
    CHECK(inst->mesh_count >= 1);
    CHECK(world.get<NameComponent>(inst->root).name == "cube.gltf");
    CHECK_FALSE(instantiate_model(assets.manager, world, "samples/missing.gltf").has_value());
}

#if AE_WITH_ANIMATION
TEST_CASE("the skinned sample gets a bound, playing animator") {
    AssetFixture assets;
    World        world;
    auto         inst = instantiate_model(assets.manager, world, "samples/skinned/skinned.gltf");
    REQUIRE(inst.has_value());
    REQUIRE(inst->animator_count == 1);
    Entity skinned = kNullEntity;
    for (const Entity e : inst->nodes) {
        if (world.has<animation::AnimatorComponent>(e)) {
            skinned = e;
        }
    }
    REQUIRE((skinned != kNullEntity));
    const auto& anim = world.get<animation::AnimatorComponent>(skinned);
    CHECK(anim.skeleton_asset.is_valid());
    CHECK(anim.graph_asset.is_valid()); // the source's first clip was picked automatically
    CHECK(world.has<MeshRendererComponent>(skinned));

    AnimationAssetBinder binder(&assets.manager);
    binder.bind_world(world, /*wait=*/true);
    CHECK(binder.bound_count() == 1);
    REQUIRE(anim.skeleton() != nullptr);
    CHECK(anim.graph_instance().valid());

    // Ticking moves joints away from the bind pose.
    animation::AnimationSubsystem sys;
    sys.update(world, 0.25f);
    const auto palette = sys.palette(skinned);
    REQUIRE(palette.size() == anim.skeleton()->joint_count());
    bool moved = false;
    for (const Mat4& m : palette) {
        moved = moved || !(m == Mat4(1.0f));
    }
    CHECK(moved);
}
#endif
