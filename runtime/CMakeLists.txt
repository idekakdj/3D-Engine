# runtime/ — the Aether runtime player (Layer 5). OWNER: runtime agent.
#   aether.runtime  static library: .aeproject manifests, packaging (cook + copy) and the
#                   PlayerApp (a slim gameplay::Application: no ImGui, cooked assets).
#   aether-player   the executable (app/main.cpp).
if(NOT (TARGET aether.gameplay AND TARGET aether.physics AND TARGET aether.scripting))
    message(STATUS "Aether: runtime skipped (needs gameplay, physics and scripting)")
    return()
endif()

aether_add_module(runtime
    SOURCES
        src/project.cpp
        src/player.cpp
        src/workspace.cpp
    PUBLIC_DEPS
        aether::core
        aether::gameplay
    PRIVATE_DEPS
        aether::assets
        aether::scene
        aether::physics
        aether::scripting
        glfw
        nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(aether.runtime PRIVATE shell32 ole32) # SHGetKnownFolderPath (workspace.cpp)
endif()

add_executable(aether-player app/main.cpp)
target_link_libraries(aether-player PRIVATE aether::runtime)
ae_set_target_warnings(aether-player)
if(MSVC)
    target_compile_options(aether-player PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-player PROPERTIES FOLDER "apps")

aether_add_test(runtime
    tests/test_project.cpp
    tests/test_workspace.cpp)
if(TARGET test.runtime)
    target_link_libraries(test.runtime PRIVATE aether::assets)
    target_compile_definitions(test.runtime PRIVATE
        AE_TEST_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")
endif()
