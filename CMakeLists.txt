cmake_minimum_required(VERSION 3.25)

project(Aether
    VERSION 0.1.0
    DESCRIPTION "Aether Engine - a C++20 + Vulkan real-time 3D engine"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Language / standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Static libraries do not link, so building `aether.X` must not force its static-lib
# dependencies to build first. Lets parallel agents compile-check their own module
# while a sibling module is mid-edit (ADR-0002).
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

# ---------------------------------------------------------------------------
# Options (ADR-0001: /WX opt-in, warnings target-scoped, no global flag mutation)
# ---------------------------------------------------------------------------
option(AE_WERROR             "Treat warnings as errors (opt-in)" OFF)
option(AE_BUILD_TESTS        "Build unit tests"                  ON)
option(AE_ENABLE_VALIDATION  "Enable Vulkan validation if the layer is discoverable" ON)
option(AE_ENABLE_TRACY       "Enable Tracy profiler"             OFF)

# Module filter (ADR-0002). Empty = every module whose CMakeLists.txt exists.
# WIP presets restrict this so one agent's in-flux module can't break another's configure.
set(AE_MODULES "" CACHE STRING "Semicolon list of modules/apps to include (empty = all present)")

# ae_module_enabled(<name> <dir-relative-to-root> <out-var>)
function(ae_module_enabled name dir out)
    set(_on FALSE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${dir}/CMakeLists.txt")
        if(NOT AE_MODULES OR name IN_LIST AE_MODULES)
            set(_on TRUE)
        endif()
    endif()
    set(${out} ${_on} PARENT_SCOPE)
endfunction()

# All binaries land together (DLLs beside EXEs on Windows).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Global, non-conformance-affecting flags only (safe for third-party too).
if(MSVC)
    add_compile_options(/MP /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Helpers + dependencies
# ---------------------------------------------------------------------------
include(CompilerWarnings)   # ae_set_target_warnings()
include(Dependencies)       # FetchContent for all third-party (gated per enabled module)
include(AetherModule)       # aether_add_module(), aether_add_test()
include(ShaderCompile)      # offline shader stub (runtime compile is the M0/M1 path)

# enable_testing() must precede add_subdirectory so module tests register with ctest.
if(AE_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Engine modules (layer order), then apps, then samples.
# ---------------------------------------------------------------------------
set(AE_ENGINE_MODULES core rhi renderer scene assets physics animation scripting gameplay)
foreach(m IN LISTS AE_ENGINE_MODULES)
    ae_module_enabled(${m} "engine/${m}" _enabled)
    if(_enabled)
        add_subdirectory("engine/${m}")
        message(STATUS "Aether: + engine/${m}")
    endif()
endforeach()

# runtime before editor: the editor opens .aeproject workspaces through aether.runtime (ADR-0013).
foreach(app IN ITEMS runtime editor samples)
    ae_module_enabled(${app} "${app}" _enabled)
    if(_enabled)
        add_subdirectory("${app}")
        message(STATUS "Aether: + ${app}")
    endif()
endforeach()

# Golden-image render tests (tests/golden) — part of AE_MODULES as "golden".
ae_module_enabled(golden "tests/golden" _enabled)
if(_enabled)
    add_subdirectory("tests/golden")
    message(STATUS "Aether: + tests/golden")
endif()

# Install rules + CPack packages (ADR-0013).
include(Packaging)

message(STATUS "Aether ${PROJECT_VERSION} configured (build type: ${CMAKE_BUILD_TYPE}, modules: ${AE_MODULES})")
