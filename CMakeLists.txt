# Tekken Tag Tournament 2 Native Online - build system root.
#
# This tree contains only first-party tooling and (later) the native host
# application. It never contains game code or data: recompiled game code is
# generated on the player's machine from their own copy (see docs/LEGAL.md).

cmake_minimum_required(VERSION 3.25)

project(ttt2_native_online
    VERSION 0.1.0
    DESCRIPTION "Native Windows port of Tekken Tag Tournament 2 (Xbox 360) with rollback netcode - tooling and host"
    LANGUAGES CXX
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TTT2_BUILD_TESTS "Build the unit tests" ON)
option(TTT2_BUILD_TOOLS "Build the command-line tools" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ttt2_warnings)

# Version header with the git description (empty string outside a git checkout).
find_package(Git QUIET)
set(TTT2_GIT_DESCRIBE "unknown")
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE TTT2_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result)
    if(NOT _git_result EQUAL 0)
        set(TTT2_GIT_DESCRIBE "unknown")
    endif()
endif()
configure_file(cmake/project_version.hpp.in
    "${PROJECT_BINARY_DIR}/generated/include/ttt2/core/project_version.hpp" @ONLY)

add_subdirectory(src/core)
add_subdirectory(src/xex)
add_subdirectory(src/xdvdfs)
add_subdirectory(src/stfs)

if(TTT2_BUILD_TOOLS)
    add_subdirectory(src/tools)
endif()

if(TTT2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(third_party/doctest)
    add_subdirectory(tests)
endif()

message(STATUS "TTT2 Native Online ${PROJECT_VERSION} (${TTT2_GIT_DESCRIBE}): "
               "compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, "
               "tests=${TTT2_BUILD_TESTS}, tools=${TTT2_BUILD_TOOLS}, Werror=${TTT2_WARNINGS_AS_ERRORS}")
