# cmake/deps/rhi.cmake - VMA, glslang (runtime GLSL->SPIR-V), Dear ImGui docking + GLFW/Vulkan backends
# OWNER: the rhi module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.

FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _none_          # header-only; skip its CMake
    SYSTEM)

# glslang: optimizer OFF (drops SPIRV-Tools), HLSL OFF, no CLI binaries.
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        15.3.0
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8-docking
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(vma glslang imgui)

add_library(aether_vma INTERFACE)
target_include_directories(aether_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
target_link_libraries(aether_vma INTERFACE volk)

# One interface target for glslang so modules don't care about its internal layout.
add_library(aether_glslang INTERFACE)
foreach(t glslang::glslang glslang::glslang-default-resource-limits glslang::SPIRV
          glslang glslang-default-resource-limits SPIRV)
    if(TARGET ${t})
        target_link_libraries(aether_glslang INTERFACE ${t})
    endif()
endforeach()
target_include_directories(aether_glslang SYSTEM INTERFACE "${glslang_SOURCE_DIR}")

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC glfw volk)
set_target_properties(imgui PROPERTIES FOLDER "third_party")
