function(mh_set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(MH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # Restrict to C/C++ so resource scripts are not handed compiler-only
        # flags llvm-windres does not understand.
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>)
        if(MH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:C,CXX>:-Werror>)
        endif()
    endif()
endfunction()

function(mh_make_portable_executable target)
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
    endif()
endfunction()
