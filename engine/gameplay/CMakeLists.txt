# aether.gameplay — the engine ASSEMBLY layer (Layer 4b, ADR-0003).
#
# Owns the Application (services + frame loop + subsystem registry), the asset->render bridge
# (AssetManager -> Renderer registration, ECS -> RenderScene), input action mapping, camera
# controllers, scene instantiation, module component codecs and the Layer-4a Lua bindings.
#
# Optional Layer-4a modules are integrated only when their targets exist, so gameplay builds
# before (and without) them:
#   aether.animation -> AE_WITH_ANIMATION=1   (AnimationSubsystem, skinning palettes)
#   aether.scripting -> AE_WITH_SCRIPTING=1   (ScriptingSubsystem, physics Lua bindings)

set(_gameplay_private_deps aether::physics imgui nlohmann_json::nlohmann_json)
set(_gameplay_private_defines)

if(TARGET aether.animation)
    list(APPEND _gameplay_private_deps aether::animation)
    list(APPEND _gameplay_private_defines AE_WITH_ANIMATION=1)
else()
    list(APPEND _gameplay_private_defines AE_WITH_ANIMATION=0)
endif()

if(TARGET aether.scripting)
    list(APPEND _gameplay_private_deps aether::scripting)
    list(APPEND _gameplay_private_defines AE_WITH_SCRIPTING=1)
else()
    list(APPEND _gameplay_private_defines AE_WITH_SCRIPTING=0)
endif()

aether_add_module(gameplay
    SOURCES
        src/application.cpp
        src/command_line.cpp
        src/input_map.cpp
        src/camera_controller.cpp
        src/render_bridge.cpp
        src/animation_bridge.cpp
        src/scene_instantiation.cpp
        src/component_codecs.cpp
        src/procedural_mesh.cpp
        src/asset_hot_reload.cpp
        src/lua_bindings.cpp
        src/imgui_panels.cpp
    PUBLIC_DEPS
        aether::core
        aether::scene
        aether::rhi
        aether::renderer
        aether::assets
    PRIVATE_DEPS
        ${_gameplay_private_deps}
    PRIVATE_DEFINES
        ${_gameplay_private_defines})

aether_add_test(gameplay
    tests/test_command_line.cpp
    tests/test_input_map.cpp
    tests/test_fixed_step.cpp
    tests/test_render_bridge.cpp
    tests/test_scene_instantiation.cpp
    tests/test_component_codecs.cpp
    tests/test_camera_controller.cpp)

if(TARGET test.gameplay)
    # Tests build physics components / query the private test seams.
    target_link_libraries(test.gameplay PRIVATE aether::physics nlohmann_json::nlohmann_json)
    target_include_directories(test.gameplay PRIVATE src)
    target_compile_definitions(test.gameplay PRIVATE ${_gameplay_private_defines})
endif()
