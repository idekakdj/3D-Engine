# aether.core - Layer 1: vocabulary types, logging, assertions, time, paths, jobs,
# window + input (GLFW is a private implementation detail; glm is the public math backend).
aether_add_module(core
    SOURCES
        src/assert.cpp
        src/log.cpp
        src/time.cpp
        src/handle.cpp
        src/paths.cpp
        src/job_system.cpp
        src/input.cpp
        src/window_glfw.cpp
    PUBLIC_DEPS
        glm::glm
    PRIVATE_DEPS
        glfw
        volk
    PUBLIC_DEFINES
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL)

# Dev fallback for paths::engine_root(): build trees live OUTSIDE the repo
# (C:/Users/<user>/.aether/build/<preset>), so the exe-relative search cannot find
# `shaders/`. The repo path contains a space - the escaped quotes keep it one token.
target_compile_definitions(aether.core PRIVATE "AE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

aether_add_test(core
    tests/core_tests.cpp)
if(TARGET test.core)
    # White-box tests (input edge logic) include private headers.
    target_include_directories(test.core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
