# cmake/deps/editor.cmake - ImGuizmo (transform gizmos) for the editor.
# OWNER: the editor agent may edit this file to fix integration of its own dependencies.
# Included by cmake/Dependencies.cmake only when the editor is enabled. ImGui itself comes from
# deps/rhi.cmake (the `imgui` target).

FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        18cef5e031d8c6973d80284c67f60549fafd78c1 # master 2026-08-08 (MIT)
    SOURCE_SUBDIR  _none_
    SYSTEM)
FetchContent_MakeAvailable(imguizmo)

add_library(aether_imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(aether_imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(aether_imguizmo PUBLIC imgui)
set_target_properties(aether_imguizmo PROPERTIES FOLDER "third_party")
