# Target-scoped warnings only (ADR-0001). Never mutates global CMAKE_CXX_FLAGS,
# so FetchContent third-party (declared SYSTEM) is never subjected to our /W4.
function(ae_set_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /w14640   # thread-unsafe static member init
            /wd4201   # nonstandard: nameless struct/union (glm, math)
            /wd4324   # structure padded due to alignas
        )
        if(AE_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-nested-anon-types)
        if(AE_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
