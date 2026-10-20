# Headless host for recompiled test programs (Linux and Windows, no window, no GPU).
#
# Builds the ReXGlue runtime from the pinned SDK checkout plus the C++ that `rexglue codegen`
# generated for one license-free test program (recomp/tests/ppc), and links them into
# `headless_host`, which runs the program's entry point on a guest thread and checks the values it
# stored in guest memory. scripts/codegen-smoke-test.sh --exec drives it. No game content involved.
#
#   cmake -S recomp/tests/host -B out/host-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 ...
#         -DREXSDK_DIR=third_party/rexglue-sdk/src
#         -DTTT2_GUEST_PROJECT=switch_ttt2 -DTTT2_GUEST_GENERATED=out/codegen-smoke/switch_ttt2/generated/default
cmake_minimum_required(VERSION 3.25)
project(ttt2_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "" CACHE PATH "ReXGlue SDK source tree (scripts/fetch-rexglue-sdk.sh)")
set(TTT2_GUEST_PROJECT "" CACHE STRING "codegen project name: the prefix of the generated files")
set(TTT2_GUEST_GENERATED "" CACHE PATH "directory holding the generated code (sources.cmake)")
foreach(var REXSDK_DIR TTT2_GUEST_PROJECT TTT2_GUEST_GENERATED)
    if(NOT ${var})
        message(FATAL_ERROR "${var} must be set")
    endif()
endforeach()
get_filename_component(REXSDK_DIR "${REXSDK_DIR}" ABSOLUTE)
get_filename_component(TTT2_GUEST_GENERATED "${TTT2_GUEST_GENERATED}" ABSOLUTE)
if(NOT EXISTS "${TTT2_GUEST_GENERATED}/sources.cmake")
    message(FATAL_ERROR "no generated code in ${TTT2_GUEST_GENERATED} (run rexglue codegen first)")
endif()

# The SDK's own options: no profiler, no perf counters (as scripts/build-rexglue-linux.sh).
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REXSDK_DIR}" rexglue-sdk EXCLUDE_FROM_ALL)

include("${TTT2_GUEST_GENERATED}/sources.cmake")

# The host's kernel stub counter, kernel overrides and export wrappers (recomp/src) are compiled in, so
# the harness reports the stubs a program reaches and binds import calls exactly as the game host does:
# a program that calls an overridden export (recomp/tests/ppc/imports_ttt2.s) gets the override's result,
# one that queries a file's disc sector (io_ttt2.s) gets the host's answer.
set(TTT2_HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_executable(headless_host headless_main.cpp "${TTT2_HOST_SRC}/stub_counter.cpp"
    "${TTT2_HOST_SRC}/kernel_overrides.cpp" "${TTT2_HOST_SRC}/runtime_exports.cpp"
    "${TTT2_HOST_SRC}/boot_monitor.cpp" "${TTT2_HOST_SRC}/io_overrides.cpp" "${TTT2_HOST_SRC}/wait_overrides.cpp"
    "${TTT2_HOST_SRC}/milestone_overrides.cpp" ${GENERATED_SOURCES})
target_compile_definitions(headless_host PRIVATE
    TTT2_GUEST_INIT_HEADER="${TTT2_GUEST_PROJECT}_init.h"
    TTT2_GUEST_PROJECT_NAME="${TTT2_GUEST_PROJECT}")
target_include_directories(headless_host PRIVATE "${TTT2_GUEST_GENERATED}" "${TTT2_HOST_SRC}")
target_link_libraries(headless_host PRIVATE rex::runtime ${CMAKE_DL_LIBS})
target_precompile_headers(headless_host PRIVATE "${TTT2_GUEST_GENERATED}/${TTT2_GUEST_PROJECT}_pch.h")
rexglue_apply_target_settings(headless_host)
# Next to librexruntime.so / rexruntime.dll so the loader finds it without an install step.
set_target_properties(headless_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH ON)
