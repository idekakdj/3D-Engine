# engine/animation — skeletal animation runtime (Layer 4). OWNER: animation agent.
# Depends on assets (data types only), scene (ECS + serializer codecs) and core. Never on renderer.
aether_add_module(animation
    SOURCES
        src/skeleton.cpp
        src/pose.cpp
        src/clip.cpp
        src/root_motion.cpp
        src/ik.cpp
        src/blend_space.cpp
        src/anim_graph.cpp
        src/anim_graph_instance.cpp
        src/components.cpp
        src/animation_subsystem.cpp
        src/serialization.cpp
    PUBLIC_DEPS aether::core aether::scene aether::assets
    PRIVATE_DEPS nlohmann_json::nlohmann_json)

aether_add_test(animation
    tests/test_skeleton_pose.cpp
    tests/test_clip.cpp
    tests/test_graph.cpp
    tests/test_ik_root_motion.cpp
    tests/test_animator.cpp)
