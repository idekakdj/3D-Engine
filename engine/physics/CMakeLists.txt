# aether.physics — Jolt Physics integration (Layer 4). Jolt stays PRIVATE: no Jolt type appears in
# include/aether/physics/*, and Jolt's PUBLIC compile definitions/options (JPH_*, /arch:AVX2) only
# apply to this module's own translation units.
aether_add_module(physics
    SOURCES
        src/runtime.cpp
        src/job_system_adapter.cpp
        src/shape_builder.cpp
        src/contact_tracker.cpp
        src/physics_world.cpp
        src/physics_queries.cpp
        src/physics_debug.cpp
        src/physics_subsystem.cpp
    PUBLIC_DEPS
        aether::core
        aether::scene
    PRIVATE_DEPS
        Jolt)

if(MSVC)
    # ADR-0001 float policy: simulation code is /fp:precise, never /fp:fast (explicit on purpose).
    target_compile_options(aether.physics PRIVATE /fp:precise)
endif()

# Every TU includes the heavy Jolt headers through src/jolt_common.h.
target_precompile_headers(aether.physics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/jolt_common.h")

aether_add_test(physics tests/physics_tests.cpp)
