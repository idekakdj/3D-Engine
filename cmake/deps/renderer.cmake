# cmake/deps/renderer.cmake - renderer-owned dependencies (ADR-0009 hook, ADR-0010).
# OWNER: the renderer/graphics agent. Included by cmake/Dependencies.cmake when the renderer
# module is enabled.
#
#   meshoptimizer  MIT (LICENSE.md)  - meshlet building + bounds for the mesh-shader path.
#                  Only the library sources are compiled (no gltfpack / JS / tools).

FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        6daea4695c48338363b08022d2fb15deaef6ac09 # v0.25
    SOURCE_SUBDIR  _none_          # use our own target below
    SYSTEM)
FetchContent_MakeAvailable(meshoptimizer)

file(GLOB _aether_meshopt_sources "${meshoptimizer_SOURCE_DIR}/src/*.cpp")
add_library(aether_meshoptimizer STATIC ${_aether_meshopt_sources})
target_include_directories(aether_meshoptimizer SYSTEM PUBLIC "${meshoptimizer_SOURCE_DIR}/src")
target_compile_features(aether_meshoptimizer PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(aether_meshoptimizer PRIVATE /W0)
else()
    target_compile_options(aether_meshoptimizer PRIVATE -w)
endif()
set_target_properties(aether_meshoptimizer PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
