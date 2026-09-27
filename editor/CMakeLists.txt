# editor/ — the Aether editor (Layer 5). OWNER: editor agent.
#   aether.editor   static library: the EditorApp (panels, viewport, undo, play-in-editor) plus
#                   the GPU-free pieces unit-tested in tests/ (edit history, picking + the GPU pick
#                   fallback tracker, selection, snapping, prefabs, material instances, anim view).
#   aether-editor   the executable (app/main.cpp).
if(NOT (TARGET aether.gameplay AND TARGET aether.physics AND TARGET aether.scripting AND TARGET aether.animation
        AND TARGET aether.runtime))
    message(STATUS "Aether: editor skipped (needs gameplay, physics, scripting, animation and runtime)")
    return()
endif()

aether_add_module(editor
    SOURCES
        src/edit_history.cpp
        src/picking.cpp
        src/console.cpp
        src/selection.cpp
        src/snapping.cpp
        src/prefab.cpp
        src/material_instance.cpp
        src/anim_view.cpp
        src/thumbnail.cpp
        src/thumbnail_cache.cpp
        src/editor_app.cpp
        src/editor_actions.cpp
        src/panel_main.cpp
        src/panel_viewport.cpp
        src/panel_hierarchy.cpp
        src/panel_inspector.cpp
        src/panel_assets.cpp
        src/panel_material.cpp
        src/self_test.cpp
    PUBLIC_DEPS
        aether::core
        aether::scene
        aether::gameplay
    PRIVATE_DEPS
        aether::physics
        aether::scripting
        aether::animation
        aether::renderer
        aether::rhi
        imgui
        aether_imguizmo
        nlohmann_json::nlohmann_json
        aether_stb) # thumbnail image decoding (stb_image's implementation is in aether.assets)

add_executable(aether-editor app/main.cpp)
target_link_libraries(aether-editor PRIVATE aether::editor aether::runtime) # projects / workspace (ADR-0013)
target_include_directories(aether-editor PRIVATE src)
ae_set_target_warnings(aether-editor)
if(MSVC)
    target_compile_options(aether-editor PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-editor PROPERTIES FOLDER "apps")

aether_add_test(editor
    tests/test_edit_history.cpp
    tests/test_picking.cpp
    tests/test_selection.cpp
    tests/test_snapping.cpp
    tests/test_prefab.cpp
    tests/test_material_instance.cpp
    tests/test_anim_view.cpp
    tests/test_thumbnail.cpp)
if(TARGET test.editor)
    target_link_libraries(test.editor PRIVATE aether::animation aether::physics)
endif()
