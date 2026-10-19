# Runs one command and checks its exit code, output and side effects. Used by the end-to-end tool
# tests so a test can assert both "exits with 1" and "prints this reason" (ctest's WILL_FAIL and
# PASS_REGULAR_EXPRESSION cannot be combined that way).
#
#   cmake -DCMD=<program|arg|arg...> [-DEXPECT_RC=<n>] [-DEXPECT_RE=<regex@@regex...>]
#         [-DEXPECT_FILES=<path|path...>] -P run_expect.cmake
#
# CMD and EXPECT_FILES are '|'-separated lists, EXPECT_RE is a '@@'-separated list of CMake
# regular expressions that must all match stdout+stderr. The combined output is printed on failure.
if(NOT DEFINED CMD)
    message(FATAL_ERROR "run_expect: CMD is required")
endif()
if(NOT DEFINED EXPECT_RC)
    set(EXPECT_RC 0)
endif()

string(REPLACE "|" ";" command "${CMD}")
execute_process(COMMAND ${command} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(text "${out}${err}")
set(failed FALSE)

if(NOT rc EQUAL EXPECT_RC)
    message(STATUS "run_expect: exit code '${rc}', expected ${EXPECT_RC}")
    set(failed TRUE)
endif()
if(DEFINED EXPECT_RE)
    string(REPLACE "@@" ";" patterns "${EXPECT_RE}")
    foreach(pattern IN LISTS patterns)
        if(NOT text MATCHES "${pattern}")
            message(STATUS "run_expect: output does not match '${pattern}'")
            set(failed TRUE)
        endif()
    endforeach()
endif()
if(DEFINED EXPECT_FILES)
    string(REPLACE "|" ";" files "${EXPECT_FILES}")
    foreach(file IN LISTS files)
        if(NOT EXISTS "${file}")
            message(STATUS "run_expect: expected file is missing: ${file}")
            set(failed TRUE)
        endif()
    endforeach()
endif()

if(failed)
    message(STATUS "run_expect: command was: ${command}")
    message(STATUS "run_expect: output follows\n${text}")
    message(FATAL_ERROR "run_expect: expectations not met")
endif()
message(STATUS "run_expect: ok (exit ${rc})")
