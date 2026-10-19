# doctest - single-header unit test framework (MIT). See README.md in this directory.
#
# The header is not vendored (it is 320 KB). It is downloaded once at configure
# time with a pinned version and SHA-256, into the build tree. For offline
# builds, drop a copy of doctest.h next to this file (it is git-ignored).

set(TTT2_DOCTEST_VERSION "2.4.11")
set(TTT2_DOCTEST_SHA256 "44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb")

set(_doctest_local "${CMAKE_CURRENT_SOURCE_DIR}/doctest.h")
if(EXISTS "${_doctest_local}")
    set(_doctest_include_dir "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(_doctest_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
    set(_doctest_header "${_doctest_include_dir}/doctest.h")
    if(NOT EXISTS "${_doctest_header}")
        file(MAKE_DIRECTORY "${_doctest_include_dir}")
        message(STATUS "Downloading doctest ${TTT2_DOCTEST_VERSION} (pinned SHA-256)")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/doctest/doctest/v${TTT2_DOCTEST_VERSION}/doctest/doctest.h"
            "${_doctest_header}"
            EXPECTED_HASH SHA256=${TTT2_DOCTEST_SHA256}
            TLS_VERIFY ON
            STATUS _doctest_status)
        list(GET _doctest_status 0 _doctest_code)
        if(NOT _doctest_code EQUAL 0)
            file(REMOVE "${_doctest_header}")
            message(FATAL_ERROR
                "Could not download doctest.h (${_doctest_status}). Either allow network access during "
                "configure, or place doctest v${TTT2_DOCTEST_VERSION} doctest.h in third_party/doctest/, "
                "or configure with -DTTT2_BUILD_TESTS=OFF.")
        endif()
    endif()
endif()

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${_doctest_include_dir}")
add_library(doctest::doctest ALIAS doctest)
