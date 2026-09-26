# cmake/deps/physics.cmake - Jolt Physics
# OWNER: the physics module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.
#
# Every JPH_* compile definition (and /arch:AVX2) is PUBLIC on the `Jolt` target, so any TU
# that links Jolt sees exactly the configuration Jolt was compiled with. aether.physics links
# Jolt PRIVATELY: none of this leaks to consumers of aether::physics.

set(TARGET_UNIT_TESTS                 OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD                OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST           OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES                    OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER                     OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS               OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL                    OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY   OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS                OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION      OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)

# ADR-0001 float policy: simulation code never builds with /fp:fast. Without this option Jolt's
# MSVC flags use /fp:fast; with it Jolt builds /fp:precise and defines
# JPH_CROSS_PLATFORM_DETERMINISTIC, which also makes replays/lockstep networking portable.
set(CROSS_PLATFORM_DETERMINISTIC      ON  CACHE BOOL "" FORCE)

# Debug drawing is done by aether.physics itself (physics::DebugLine), and profiling will go
# through the engine profiler (JPH_USE_EXTERNAL_PROFILE) once it exists; the built-in versions
# only add per-config ODR skew (they are enabled for Debug/Release but not RelWithDebInfo).
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE       OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM                OFF CACHE BOOL "" FORCE)

# Jolt's internal asserts catch API misuse (wrong thread, invalid body IDs, ...). They are
# routed to the engine log by aether.physics. Debug builds only.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(USE_ASSERTS ON  CACHE BOOL "" FORCE)
else()
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.5.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    SYSTEM)
FetchContent_MakeAvailable(JoltPhysics)
if(TARGET Jolt)
    set_target_properties(Jolt PROPERTIES FOLDER "third_party")
endif()
