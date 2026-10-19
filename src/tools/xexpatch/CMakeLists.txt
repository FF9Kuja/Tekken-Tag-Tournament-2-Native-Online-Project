# ttt2-xexpatch: applies an Xbox 360 title-update delta patch (.xexp) to a base .xex.
#
# Built on XenonUtils' XexPatcher (XenonRecomp, MIT; patcher derived from Xenia Canary, BSD-3),
# libmspack's LZX decoder (LGPL-2.1) and tiny-AES-c (Unlicense). Only those sources are compiled:
# XenonRecomp's GPL PowerPC disassembler is never touched. The checkout comes from
# scripts/fetch-xenonrecomp.sh; without it this tool is skipped with a status message.

set(TTT2_XENONRECOMP_DIR "${PROJECT_SOURCE_DIR}/third_party/xenonrecomp/src"
    CACHE PATH "Pinned XenonRecomp checkout (scripts/fetch-xenonrecomp.sh)")
set(_xu "${TTT2_XENONRECOMP_DIR}/XenonUtils")
set(_tp "${TTT2_XENONRECOMP_DIR}/thirdparty")

if(EXISTS "${_xu}/xex_patcher.cpp" AND EXISTS "${_tp}/libmspack/libmspack/mspack/lzxd.c"
   AND EXISTS "${_tp}/tiny-AES-c/aes.c" AND EXISTS "${_tp}/TinySHA1/TinySHA1.hpp")
    enable_language(C)
    add_library(ttt2_xexpatcher STATIC
        "${_xu}/xex_patcher.cpp"
        "${_xu}/memory_mapped_file.cpp"
        "${_tp}/libmspack/libmspack/mspack/lzxd.c"
        "${_tp}/tiny-AES-c/aes.c"
    )
    target_include_directories(ttt2_xexpatcher SYSTEM PUBLIC
        "${_xu}"
        "${_tp}/libmspack/libmspack/mspack"
        "${_tp}/tiny-AES-c"
        "${_tp}/TinySHA1"
    )
    target_compile_definitions(ttt2_xexpatcher PRIVATE NOMINMAX)
    if(MSVC)
        target_compile_options(ttt2_xexpatcher PRIVATE /w)
    else()
        target_compile_options(ttt2_xexpatcher PRIVATE -w)
    endif()
    set_target_properties(ttt2_xexpatcher PROPERTIES CXX_STANDARD 20)

    add_executable(ttt2-xexpatch main.cpp)
    target_link_libraries(ttt2-xexpatch PRIVATE ttt2_xexpatcher ttt2::xex ttt2::core)
    ttt2_apply_warnings(ttt2-xexpatch)
    install(TARGETS ttt2-xexpatch RUNTIME DESTINATION bin)
    message(STATUS "ttt2-xexpatch: enabled (XenonUtils at ${TTT2_XENONRECOMP_DIR})")
else()
    message(STATUS "ttt2-xexpatch: skipped - run scripts/fetch-xenonrecomp.sh to enable")
endif()
