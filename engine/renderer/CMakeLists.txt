# aether.renderer - render graph + clustered forward+ PBR renderer (blueprint §7, ADR-0002).
# Depends only on core + rhi (+ meshoptimizer, private, for the ADR-0010 meshlet path). Shaders
# live in shaders/renderer/: loaded from the SPIR-V cache or compiled at runtime (ADR-0014).

aether_add_module(renderer
    SOURCES
        src/frame_arena.cpp
        src/geometry_arena.cpp
        src/ibl.cpp
        src/meshlets.cpp
        src/pipelines.cpp
        src/range_allocator.cpp
        src/render_graph.cpp
        src/render_math.cpp
        src/renderer_frame.cpp
        src/renderer_impl.cpp
        src/shadow_math.cpp
    PUBLIC_DEPS
        aether::core
        aether::rhi
    PRIVATE_DEPS
        aether_meshoptimizer
    PRIVATE_DEFINES
        # Dev fallback when paths::shader_dir() (exe-relative) has no renderer/ folder.
        AE_RENDERER_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders")

aether_add_test(renderer
    tests/render_graph_tests.cpp
    tests/math_tests.cpp
    tests/resource_tests.cpp
    tests/shader_tests.cpp
    tests/renderer_mock_tests.cpp
    tests/texture_tests.cpp
    tests/gpu_driven_tests.cpp
    tests/meshlet_tests.cpp
    tests/shader_cache_tests.cpp)

if(TARGET test.renderer)
    # Tests exercise private implementation headers (render graph, culling, ...).
    target_include_directories(test.renderer PRIVATE src tests)
    target_link_libraries(test.renderer PRIVATE aether_meshoptimizer)
    target_compile_definitions(test.renderer PRIVATE
        AE_RENDERER_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders")
endif()

# ADR-0014: aether-shaderc bakes every shader permutation into ${CMAKE_BINARY_DIR}/shader_spirv
# (installed as shaders/spirv) so installed builds start without compiling GLSL. Re-runs when any
# file under shaders/ changes; the apps depend on aether_shader_cache (see cmake/Packaging.cmake).
add_executable(aether-shaderc tools/shaderc/main.cpp)
target_include_directories(aether-shaderc PRIVATE src)
target_link_libraries(aether-shaderc PRIVATE aether::renderer)
ae_set_target_warnings(aether-shaderc)
if(MSVC)
    target_compile_options(aether-shaderc PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-shaderc PROPERTIES FOLDER "tools")

file(GLOB_RECURSE _ae_shader_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*")
set(AE_SHADER_SPIRV_DIR "${CMAKE_BINARY_DIR}/shader_spirv" CACHE INTERNAL "Baked SPIR-V cache (ADR-0014)")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/shader_spirv.stamp"
    COMMAND aether-shaderc "${CMAKE_SOURCE_DIR}/shaders" "${AE_SHADER_SPIRV_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/shader_spirv.stamp"
    DEPENDS aether-shaderc ${_ae_shader_files}
    COMMENT "Baking the SPIR-V shader cache"
    VERBATIM)
add_custom_target(aether_shader_cache DEPENDS "${CMAKE_BINARY_DIR}/shader_spirv.stamp")
set_target_properties(aether_shader_cache PROPERTIES FOLDER "tools")
