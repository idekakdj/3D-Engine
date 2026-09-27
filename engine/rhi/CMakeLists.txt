# aether.rhi - Layer 2: render hardware interface + Vulkan 1.3 backend, runtime GLSL
# compiler (glslang) and the Dear ImGui GLFW/Vulkan backend (ADR-0001 escape hatch).
# Every Vulkan / VMA / glslang / ImGui type stays private to this module.
aether_add_module(rhi
    SOURCES
        src/vk_convert.cpp
        src/vk_debug.cpp
        src/vk_device.cpp
        src/vk_swapchain.cpp
        src/vk_frame.cpp
        src/vk_resources.cpp
        src/vk_bindless.cpp
        src/vk_pipeline_cache.cpp
        src/vk_command_list.cpp
        src/vk_readback.cpp
        src/vma_impl.cpp
        src/shader_compiler.cpp
        src/imgui_backend.cpp
    PUBLIC_DEPS
        aether::core
    PRIVATE_DEPS
        volk
        aether_vma
        aether_glslang
        imgui
        glfw)

aether_add_test(rhi
    tests/rhi_tests.cpp)
if(TARGET test.rhi)
    # White-box tests (handle pool, ResourceState table) include private headers,
    # which in turn see Vulkan types through volk.
    target_include_directories(test.rhi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test.rhi PRIVATE volk aether_vma)
endif()
