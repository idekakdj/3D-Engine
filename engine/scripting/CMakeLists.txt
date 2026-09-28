# aether.scripting — Lua 5.4 + sol2 scripting (Layer 4a). OWNER: scripting agent.
#
# sol2/Lua stay PRIVATE: only the opt-in escape hatch `lua_integration.h` exposes them, and
# consumers of that header link `aether::scripting_lua`, which carries sol2 with EXACTLY this
# module's configuration (SOL_ALL_SAFETIES_ON=1 via aether_sol2) so every sol2 TU agrees (ODR).
aether_add_module(scripting
    SOURCES
        src/script_vm.cpp
        src/bindings.cpp
        src/components.cpp
        src/scripting_subsystem.cpp
        src/visual_script.cpp
    PUBLIC_DEPS
        aether::core
        aether::scene
    PRIVATE_DEPS
        aether_sol2
        nlohmann_json::nlohmann_json)

if(MSVC)
    # sol2 usertype instantiations exceed the default COFF section limit.
    target_compile_options(aether.scripting PRIVATE /bigobj)
endif()

# Opt-in sol2 access for Layer-4b/5 glue (see include/aether/scripting/lua_integration.h).
add_library(aether.scripting_lua INTERFACE)
target_link_libraries(aether.scripting_lua INTERFACE aether::scripting aether_sol2)
if(MSVC)
    target_compile_options(aether.scripting_lua INTERFACE /bigobj)
endif()
add_library(aether::scripting_lua ALIAS aether.scripting_lua)

aether_add_test(scripting
    tests/test_vm.cpp
    tests/test_bindings.cpp
    tests/test_components.cpp
    tests/test_visual_script.cpp)

if(TARGET test.scripting)
    target_link_libraries(test.scripting PRIVATE aether::scripting_lua)
    # The sample-script smoke test runs the shipped content/scripts in place.
    target_compile_definitions(test.scripting PRIVATE
        AE_TEST_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/content/scripts")
endif()
