# tests/golden — golden-image render tests (blueprint §2 / §12: renderer features get one).
#   aether.golden   static library: image comparison, diff images, PNG I/O, device classes.
#   aether-golden   the harness (app/main.cpp): renders one case offscreen and compares it with
#                   reference/<device class>/<case>.png.
#   test.golden     unit tests of the comparison library (no GPU).
# The per-case ctest entries (golden.<case>) need a Vulkan device and a display; they are
# registered when AE_GOLDEN_TESTS is ON (headless: Xvfb + Mesa llvmpipe, see README).
if(NOT (TARGET aether.gameplay AND TARGET aether.assets))
    message(STATUS "Aether: golden tests skipped (need gameplay and assets)")
    return()
endif()

option(AE_GOLDEN_TESTS "Register the GPU golden-image cases with ctest" OFF)

aether_add_module(golden
    SOURCES
        src/image_compare.cpp
    PUBLIC_DEPS
        aether::core
    PRIVATE_DEPS
        aether::assets   # stb_image's implementation (image_importer.cpp)
        aether_stb)
set_target_properties(aether.golden PROPERTIES FOLDER "tests")

add_executable(aether-golden app/main.cpp)
target_link_libraries(aether-golden PRIVATE aether::golden aether::gameplay)
ae_set_target_warnings(aether-golden)
if(MSVC)
    target_compile_options(aether-golden PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-golden PROPERTIES FOLDER "tests")

aether_add_test(golden tests/test_image_compare.cpp)

set(AE_GOLDEN_CASES
    pbr_spheres pbr_spheres_no_post punctual_lights emissive_bloom translucency
    debug_normals debug_albedo debug_roughness spot_shadows point_shadows gi_room gi_room_off reflections reflections_off shadows)
if(AE_BUILD_TESTS AND AE_GOLDEN_TESTS)
    foreach(c IN LISTS AE_GOLDEN_CASES)
        add_test(NAME golden.${c} COMMAND aether-golden --case ${c})
        set_tests_properties(golden.${c} PROPERTIES SKIP_RETURN_CODE 77 LABELS "gpu;golden" TIMEOUT 300)
    endforeach()
    # ADR-0010: the vertex (indexed indirect) path must match the same references as the meshlet
    # path on mesh-shading devices; on other devices both runs take the vertex path.
    foreach(c IN ITEMS pbr_spheres punctual_lights debug_normals)
        add_test(NAME golden.${c}.no_mesh_shading COMMAND aether-golden --case ${c} --no-mesh-shading)
        set_tests_properties(golden.${c}.no_mesh_shading PROPERTIES SKIP_RETURN_CODE 77 LABELS "gpu;golden" TIMEOUT 300)
    endforeach()
endif()
