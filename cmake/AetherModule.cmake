# aether_add_module(<name>
#     [TYPE STATIC|INTERFACE]
#     [SOURCES <files...>]
#     [PUBLIC_DEPS <targets...>]      # propagated to consumers
#     [PRIVATE_DEPS <targets...>]     # implementation-only (kept off the public ABI)
#     [PUBLIC_DEFINES <defs...>]
#     [PRIVATE_DEFINES <defs...>])
#
# Produces target `aether.<name>` and alias `aether::<name>`.
# Public headers live in ./include, private in ./src (ADR-0001 / blueprint §4.1).
function(aether_add_module NAME)
    set(options)
    set(oneValue TYPE)
    set(multiValue SOURCES PUBLIC_DEPS PRIVATE_DEPS PUBLIC_DEFINES PRIVATE_DEFINES)
    cmake_parse_arguments(ARG "${options}" "${oneValue}" "${multiValue}" ${ARGN})

    if(NOT ARG_TYPE)
        set(ARG_TYPE STATIC)
    endif()

    set(target "aether.${NAME}")

    if(ARG_TYPE STREQUAL "INTERFACE")
        add_library(${target} INTERFACE)
        target_include_directories(${target} INTERFACE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
        target_compile_features(${target} INTERFACE cxx_std_20)
        if(ARG_PUBLIC_DEPS)
            target_link_libraries(${target} INTERFACE ${ARG_PUBLIC_DEPS})
        endif()
        if(ARG_PUBLIC_DEFINES)
            target_compile_definitions(${target} INTERFACE ${ARG_PUBLIC_DEFINES})
        endif()
    else()
        add_library(${target} ${ARG_TYPE} ${ARG_SOURCES})
        target_include_directories(${target}
            PUBLIC  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_features(${target} PUBLIC cxx_std_20)

        if(ARG_PUBLIC_DEPS)
            target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS})
        endif()
        if(ARG_PRIVATE_DEPS)
            target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
        endif()
        if(ARG_PUBLIC_DEFINES)
            target_compile_definitions(${target} PUBLIC ${ARG_PUBLIC_DEFINES})
        endif()
        if(ARG_PRIVATE_DEFINES)
            target_compile_definitions(${target} PRIVATE ${ARG_PRIVATE_DEFINES})
        endif()

        ae_set_target_warnings(${target})
        if(MSVC)
            # Conformance flags kept target-scoped so they never hit third-party.
            target_compile_options(${target} PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
        endif()
        set_target_properties(${target} PROPERTIES FOLDER "engine")
    endif()

    add_library(aether::${NAME} ALIAS ${target})
endfunction()

# aether_add_test(<module> <sources...>) - a doctest executable `test.<module>` registered
# with ctest. A generated doctest main is linked in, so test sources just
# `#include <doctest/doctest.h>` and write TEST_CASEs.
function(aether_add_test MODULE)
    if(NOT AE_BUILD_TESTS)
        return()
    endif()
    set(t "test.${MODULE}")
    set(_main "${CMAKE_BINARY_DIR}/generated/doctest_main.cpp")
    if(NOT EXISTS "${_main}")
        file(WRITE "${_main}" "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
")
    endif()
    add_executable(${t} ${ARGN} "${_main}")
    target_link_libraries(${t} PRIVATE aether::${MODULE} doctest::doctest)
    ae_set_target_warnings(${t})
    if(MSVC)
        target_compile_options(${t} PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
    endif()
    set_target_properties(${t} PROPERTIES FOLDER "tests")
    add_test(NAME ${t} COMMAND ${t})
endfunction()
