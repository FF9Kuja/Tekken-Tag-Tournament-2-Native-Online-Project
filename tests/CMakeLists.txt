add_executable(ttt2_tests
    test_main.cpp
    test_endian.cpp
    test_hash.cpp
    test_aes.cpp
    test_xex2.cpp
    test_basefile.cpp
    test_patch_check.cpp
    test_imports.cpp
    test_xdvdfs.cpp
    test_stfs.cpp
    test_stub_name.cpp
)
target_link_libraries(ttt2_tests PRIVATE ttt2::core ttt2::xex ttt2::xdvdfs ttt2::stfs doctest::doctest)
# recomp/src/stub_name.hpp is header-only and SDK-free, so the host's stub-line parser is tested here.
target_include_directories(ttt2_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/recomp/src")
ttt2_apply_warnings(ttt2_tests)

add_test(NAME ttt2_unit_tests COMMAND ttt2_tests --reporters=console --no-intro)

# Python tools: the codegen report parser on synthetic output (needs python3 on the path).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME python_tools COMMAND "${Python3_EXECUTABLE}" -I -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/python"
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

# Smoke test of the tool (only when the tools are built): it must refuse a
# non-XEX file with exit code 1 and print its version with exit code 0.
if(TARGET ttt2-xexinfo)
    add_test(NAME xexinfo_version COMMAND ttt2-xexinfo --version)
    add_test(NAME xexinfo_rejects_non_xex COMMAND ttt2-xexinfo "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
    set_tests_properties(xexinfo_rejects_non_xex PROPERTIES WILL_FAIL TRUE)
endif()
if(TARGET ttt2-stfs)
    add_test(NAME stfs_version COMMAND ttt2-stfs --version)
    add_test(NAME stfs_rejects_non_package COMMAND ttt2-stfs info "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
    set_tests_properties(stfs_rejects_non_package PROPERTIES WILL_FAIL TRUE)
endif()
if(TARGET ttt2-iso)
    add_test(NAME iso_version COMMAND ttt2-iso --version)
    add_test(NAME iso_rejects_non_image COMMAND ttt2-iso info "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
    set_tests_properties(iso_rejects_non_image PROPERTIES WILL_FAIL TRUE)
endif()

# End-to-end runs of ttt2-validate on synthetic inputs: a generator writes a disc image whose
# default.xex is a synthetic title executable, a matching title-update package, a mismatching one
# and a console-style folder tree; the tool is then run the way the owner runs it. --no-patch keeps
# the runs identical with and without the Clang-only patcher. run_expect.cmake checks the exit
# code, the printed reasons and the files written to --work.
if(TARGET ttt2-validate)
    add_executable(ttt2_make_validate_fixtures tools/make_validate_fixtures.cpp)
    target_link_libraries(ttt2_make_validate_fixtures PRIVATE ttt2::xex ttt2::core)
    target_include_directories(ttt2_make_validate_fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    ttt2_apply_warnings(ttt2_make_validate_fixtures)

    set(_fx "${CMAKE_CURRENT_BINARY_DIR}/validate-fixtures")
    set(_run_expect "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_expect.cmake")
    set(_validate "$<TARGET_FILE:ttt2-validate>")
    add_test(NAME validate_fixtures COMMAND ttt2_make_validate_fixtures "${_fx}")
    set_tests_properties(validate_fixtures PROPERTIES FIXTURES_SETUP validate_inputs)

    add_test(NAME validate_synthetic_pair COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_validate}|${_fx}/synthetic.iso|${_fx}/TU_SYNTHETIC|--no-patch|--json|--work|${_fx}/work"
        -DEXPECT_RC=0
        "-DEXPECT_RE=\"ok\": true@@Pre-check\": \"passed\"@@SHA-1 of the base's headers@@Patch versions\": \"0\\.0\\.0\\.2 -> 0\\.0\\.3\\.2\"@@Base image\": \"PE image, 8192 bytes\"@@Base imports\": \"2 functions, 2 variables, 1 unnamed ordinals\"@@Base runtime support\": \"ReXGlue SDK [0-9a-f]+: 3 implemented, 0 stubs, 0 missing\"@@Base __restvmx_64\": \"0x82001300\"@@Base __savegprlr_14\": \"0x82001200 0x82001400 \\(ambiguous\\)\"@@Base layout vs TTT2 Web\": \"0 of 8 helper addresses match\"@@Patch\": \"not applied \\(--no-patch\\)\""
        "-DEXPECT_FILES=${_fx}/work/base.xex|${_fx}/work/Default.xexp"
        -P "${_run_expect}")
    add_test(NAME validate_synthetic_folder COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_validate}|${_fx}/synthetic.iso|${_fx}/folder|--no-patch"
        -DEXPECT_RC=0
        "-DEXPECT_RE=picked .*TU_SYNTHETIC@@Pre-check: +passed@@OK: the disc image and title update form a usable pair"
        -P "${_run_expect}")
    add_test(NAME validate_synthetic_mismatch COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_validate}|${_fx}/synthetic.iso|${_fx}/TU_WRONG|--no-patch"
        -DEXPECT_RC=1
        "-DEXPECT_RE=Pre-check: +FAILED@@PROBLEM: .*version mismatch@@0\\.0\\.1\\.2"
        -P "${_run_expect}")
    add_test(NAME validate_synthetic_no_update COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_validate}|${_fx}/synthetic.iso|${_fx}/folder/Content/0000000000000000/4E4D0859/00000002|--no-patch"
        -DEXPECT_RC=1
        "-DEXPECT_RE=no Title Update 3 package found@@PROBLEM: no Title Update 3 package found"
        -P "${_run_expect}")
    set_tests_properties(validate_synthetic_pair validate_synthetic_folder validate_synthetic_mismatch
                         validate_synthetic_no_update PROPERTIES FIXTURES_REQUIRED validate_inputs)

    # The whole-disc extraction the native host's game data root comes from, on the synthetic image.
    if(TARGET ttt2-iso)
        add_test(NAME iso_extract_all_synthetic COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:ttt2-iso>|extract-all|${_fx}/synthetic.iso|${_fx}/extracted"
            -DEXPECT_RC=0
            "-DEXPECT_RE=extracted 3 files \\([0-9]+ bytes\\) in 1 directories"
            "-DEXPECT_FILES=${_fx}/extracted/default.xex|${_fx}/extracted/readme.txt|${_fx}/extracted/data/table.bin"
            -P "${_run_expect}")
        set_tests_properties(iso_extract_all_synthetic PROPERTIES FIXTURES_REQUIRED validate_inputs)
    endif()
endif()
