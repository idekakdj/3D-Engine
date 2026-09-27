# aether.renderer - render graph + clustered forward+ PBR renderer (blueprint §7, ADR-0002).
# Depends only on core + rhi. Shaders live in shaders/renderer/ and are compiled at runtime.

aether_add_module(renderer
    SOURCES
        src/frame_arena.cpp
        src/geometry_arena.cpp
        src/ibl.cpp
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
    tests/gpu_driven_tests.cpp)

if(TARGET test.renderer)
    # Tests exercise private implementation headers (render graph, culling, ...).
    target_include_directories(test.renderer PRIVATE src tests)
    target_compile_definitions(test.renderer PRIVATE
        AE_RENDERER_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders")
endif()
