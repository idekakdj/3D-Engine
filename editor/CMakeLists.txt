# editor/ — the Aether editor (Layer 5). OWNER: editor agent.
#   aether.editor   static library: the EditorApp (panels, viewport, undo, play-in-editor) plus
#                   the GPU-free pieces unit-tested in tests/ (edit history, picking, console).
#   aether-editor   the executable (app/main.cpp).
if(NOT (TARGET aether.gameplay AND TARGET aether.physics AND TARGET aether.scripting AND TARGET aether.animation))
    message(STATUS "Aether: editor skipped (needs gameplay, physics, scripting and animation)")
    return()
endif()

aether_add_module(editor
    SOURCES
        src/edit_history.cpp
        src/picking.cpp
        src/console.cpp
        src/editor_app.cpp
        src/panel_main.cpp
        src/panel_viewport.cpp
        src/panel_hierarchy.cpp
        src/panel_inspector.cpp
        src/panel_assets.cpp
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
        aether_imguizmo)

add_executable(aether-editor app/main.cpp)
target_link_libraries(aether-editor PRIVATE aether::editor)
target_include_directories(aether-editor PRIVATE src)
ae_set_target_warnings(aether-editor)
if(MSVC)
    target_compile_options(aether-editor PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-editor PROPERTIES FOLDER "apps")

aether_add_test(editor
    tests/test_edit_history.cpp
    tests/test_picking.cpp)
