# All third-party dependencies, pinned to verified tags/commits.
#
# ADR-0001: declared SYSTEM so their headers never trigger our /W4.
# ADR-0002: dependencies live in the per-preset build tree (<binaryDir>/_deps) - never a
#           shared FETCHCONTENT_BASE_DIR (FetchContent puts dependency BUILD trees there
#           too, so sharing it across presets corrupts parallel builds). Optional deps are
#           fetched only when a module that needs them is enabled (see AE_MODULES).
#
# Target names exposed to engine modules (stable contract):
#   Vulkan::Headers  volk  aether_vma  glfw  glm::glm  imgui  EnTT::EnTT  doctest::doctest
#   glslang::glslang glslang::glslang-default-resource-limits (via aether_glslang)
#   Jolt  aether_lua  aether_sol2  aether_cgltf  aether_stb  nlohmann_json::nlohmann_json
#   aether_imguizmo (editor only)
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

ae_module_enabled(rhi       engine/rhi       AE_WITH_RHI)
ae_module_enabled(scene     engine/scene     AE_WITH_SCENE)
ae_module_enabled(assets    engine/assets    AE_WITH_ASSETS)
ae_module_enabled(physics   engine/physics   AE_WITH_PHYSICS)
ae_module_enabled(scripting engine/scripting AE_WITH_SCRIPTING)
ae_module_enabled(editor    editor           AE_WITH_EDITOR)

# ===========================================================================
# Always: Vulkan headers + volk (core's Window uses volk for glfwInitVulkanLoader),
#         GLFW, glm, doctest.
# ===========================================================================
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.4.328.0
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.328.0
    GIT_SHALLOW    TRUE
    SYSTEM)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    SYSTEM)

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)   # header-only
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.12
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(vulkan_headers volk glfw glm doctest)

# volk must never pull static prototypes; every consumer sees the same defines.
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
if(NOT TARGET Vulkan::Headers)
    message(FATAL_ERROR "Vulkan-Headers did not provide Vulkan::Headers")
endif()
target_link_libraries(volk PUBLIC Vulkan::Headers)

# ===========================================================================
# Optional, per-module dependency sets (each file owned by that module's agent).
# ===========================================================================
if(AE_WITH_RHI OR AE_WITH_EDITOR)
    include(deps/rhi)
endif()
if(AE_WITH_SCENE)
    include(deps/scene)
endif()
if(AE_WITH_SCENE OR AE_WITH_ASSETS)
    include(deps/json)
endif()
if(AE_WITH_ASSETS)
    include(deps/assets)
endif()
if(AE_WITH_PHYSICS)
    include(deps/physics)
endif()
if(AE_WITH_SCRIPTING)
    include(deps/scripting)
endif()
if(AE_WITH_EDITOR)
    include(deps/editor)
endif()

# Group fetched targets in IDEs.
foreach(tp glfw volk glslang SPIRV OSDependent MachineIndependent GenericCodeGen
           glslang-default-resource-limits doctest EnTT nlohmann_json)
    if(TARGET ${tp})
        get_target_property(_type ${tp} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            set_target_properties(${tp} PROPERTIES FOLDER "third_party")
        endif()
    endif()
endforeach()

message(STATUS "Aether deps: rhi=${AE_WITH_RHI} scene=${AE_WITH_SCENE} assets=${AE_WITH_ASSETS} "
               "physics=${AE_WITH_PHYSICS} scripting=${AE_WITH_SCRIPTING} editor=${AE_WITH_EDITOR}")
