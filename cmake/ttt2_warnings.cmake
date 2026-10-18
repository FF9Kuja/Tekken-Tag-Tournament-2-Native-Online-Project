# Warning configuration shared by every first-party target.
#
# TTT2_WARNINGS_AS_ERRORS defaults to OFF so that a new compiler release never
# breaks a developer's build; CI turns it on.

option(TTT2_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(ttt2_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(TTT2_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
            -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
        if(TTT2_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
