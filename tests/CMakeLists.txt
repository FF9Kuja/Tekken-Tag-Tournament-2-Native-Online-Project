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
    test_input_core.cpp
    "${PROJECT_SOURCE_DIR}/recomp/src/input/input_core.cpp"
)
target_link_libraries(ttt2_tests PRIVATE ttt2::core ttt2::xex ttt2::xdvdfs ttt2::stfs doctest::doctest)
# recomp/src/stub_name.hpp and recomp/src/input/ are SDK-free, so the host's stub-line parser and its
# keyboard input core are tested here (the host build compiles the same sources against the SDK).
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

    # The pipeline's preparation step (5c, ttt2-xexinfo --write-plain) on the retail-form synthetic executable:
    # the output must verify, be plain, carry the same image and be what the switch-table finder accepts;
    # the finder must still refuse the retail form with the message that names the step. A delta patch has
    # no plain form, and the tool must never write over its input.
    if(TARGET ttt2-xexinfo)
        set(_xexinfo "$<TARGET_FILE:ttt2-xexinfo>")
        # SHA-1 of tests/pe_image_builder.hpp's make_import_test_image(): the image inside every form of the synthetic
        # executable; the preparation step must reproduce it exactly.
        set(_image_sha1 "f1a748904bbec183df365320ebb0f2b905f896b1")
        add_test(NAME xexinfo_write_plain_synthetic COMMAND "${CMAKE_COMMAND}"
            "-DCMD=${_xexinfo}|${_fx}/retail.xex|--write-plain|${_fx}/plain/default.xex"
            -DEXPECT_RC=0
            "-DEXPECT_RE=Encryption: +normal@@Compression: +basic .zero-run blocks., 3 blocks@@Plain image: +verified, written to .*default\\.xex \\([0-9]+ bytes: 0x[0-9A-F]+ of headers kept, encryption none, compression none, PE image of 8192 bytes\\)@@Plain image SHA-1: +${_image_sha1}"
            "-DEXPECT_FILES=${_fx}/plain/default.xex"
            -P "${_run_expect}")
        add_test(NAME xexinfo_plain_output_is_plain COMMAND "${CMAKE_COMMAND}"
            "-DCMD=${_xexinfo}|${_fx}/plain/default.xex|--image-hash|--imports|--helpers"
            -DEXPECT_RC=0
            "-DEXPECT_RE=Encryption: +none@@Compression: +none@@Title ID: +4E4D0859@@Version: +0\\.0\\.0\\.2@@Image: +PE image, 8192 bytes@@Image SHA-1: +${_image_sha1}@@Imports: +2 functions, 2 variables, 1 unnamed ordinals@@__restvmx_64 +0x82001300"
            -P "${_run_expect}")
        add_test(NAME xexinfo_write_plain_refuses_input_path COMMAND "${CMAKE_COMMAND}"
            "-DCMD=${_xexinfo}|${_fx}/retail.xex|--write-plain|${_fx}/retail.xex"
            -DEXPECT_RC=1
            "-DEXPECT_RE=Plain image: +FAILED@@refusing to overwrite the input file"
            -P "${_run_expect}")
        add_test(NAME xexinfo_write_plain_rejects_delta_patch COMMAND "${CMAKE_COMMAND}"
            "-DCMD=${_xexinfo}|${_fx}/patch.xexp|--write-plain|${_fx}/plain/patch-plain.xex"
            -DEXPECT_RC=1
            "-DEXPECT_RE=Plain image: +FAILED@@delta patch"
            "-DEXPECT_MISSING=${_fx}/plain/patch-plain.xex"
            -P "${_run_expect}")
        set_tests_properties(xexinfo_write_plain_synthetic xexinfo_write_plain_refuses_input_path
                             xexinfo_write_plain_rejects_delta_patch PROPERTIES FIXTURES_REQUIRED validate_inputs)
        set_tests_properties(xexinfo_plain_output_is_plain PROPERTIES DEPENDS xexinfo_write_plain_synthetic)
        if(Python3_Interpreter_FOUND)
            set(_finder "${Python3_EXECUTABLE}|-I|${PROJECT_SOURCE_DIR}/recomp/tools/find_switch_tables.py")
            add_test(NAME finder_rejects_retail_form COMMAND "${CMAKE_COMMAND}"
                "-DCMD=${_finder}|${_fx}/retail.xex"
                -DEXPECT_RC=2
                "-DEXPECT_RE=encrypted or compressed \\(encryption 1, compression 1\\)@@ttt2-xexinfo <file\\.xex> --write-plain@@step 5c"
                -P "${_run_expect}")
            add_test(NAME finder_accepts_plain_output COMMAND "${CMAKE_COMMAND}"
                "-DCMD=${_finder}|${_fx}/plain/default.xex|--toml|${_fx}/plain/ttt2_switch_tables.toml|--json|${_fx}/plain/switch_tables.json"
                -DEXPECT_RC=0
                "-DEXPECT_RE=0 switch tables"
                "-DEXPECT_FILES=${_fx}/plain/ttt2_switch_tables.toml|${_fx}/plain/switch_tables.json"
                -P "${_run_expect}")
            set_tests_properties(finder_rejects_retail_form PROPERTIES FIXTURES_REQUIRED validate_inputs)
            set_tests_properties(finder_accepts_plain_output PROPERTIES DEPENDS xexinfo_write_plain_synthetic)
        endif()

        # The retail wrapper the SDK workflows put in front of the pipeline (plain test program -> retail form ->
        # --write-plain -> finder -> codegen -> host), round-tripped here on the plain synthetic executable.
        add_executable(ttt2_make_retail_xex tools/make_retail_xex.cpp)
        target_link_libraries(ttt2_make_retail_xex PRIVATE ttt2::xex ttt2::core)
        target_include_directories(ttt2_make_retail_xex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        ttt2_apply_warnings(ttt2_make_retail_xex)
        add_test(NAME retail_xex_wrap_roundtrip COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:ttt2_make_retail_xex>|${_fx}/plain/default.xex|${_fx}/plain/rewrapped.xex|--granule|256"
            -DEXPECT_RC=0
            "-DEXPECT_RE=encryption normal, compression basic \\(3 blocks\\), image 8192 bytes, image SHA-1 ${_image_sha1}; round trip verified"
            "-DEXPECT_FILES=${_fx}/plain/rewrapped.xex"
            -P "${_run_expect}")
        add_test(NAME retail_xex_wrap_rejects_retail_input COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:ttt2_make_retail_xex>|${_fx}/retail.xex|${_fx}/plain/never.xex"
            -DEXPECT_RC=1
            "-DEXPECT_RE=input is not plain"
            "-DEXPECT_MISSING=${_fx}/plain/never.xex"
            -P "${_run_expect}")
        add_test(NAME retail_xex_rewrapped_unwraps COMMAND "${CMAKE_COMMAND}"
            "-DCMD=${_xexinfo}|${_fx}/plain/rewrapped.xex|--write-plain|${_fx}/plain/unwrapped.xex"
            -DEXPECT_RC=0
            "-DEXPECT_RE=Encryption: +normal@@Compression: +basic .zero-run blocks., 3 blocks@@Plain image: +verified@@Plain image SHA-1: +${_image_sha1}"
            "-DEXPECT_FILES=${_fx}/plain/unwrapped.xex"
            -P "${_run_expect}")
        set_tests_properties(retail_xex_wrap_roundtrip PROPERTIES DEPENDS xexinfo_write_plain_synthetic)
        set_tests_properties(retail_xex_wrap_rejects_retail_input PROPERTIES FIXTURES_REQUIRED validate_inputs)
        set_tests_properties(retail_xex_rewrapped_unwraps PROPERTIES DEPENDS retail_xex_wrap_roundtrip)
    endif()

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
    # The title update's files extracted with their folders: the host's update: root (owner pipeline step 8).
    if(TARGET ttt2-stfs)
        add_test(NAME stfs_extract_all_synthetic COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:ttt2-stfs>|extract-all|${_fx}/TU_SYNTHETIC|${_fx}/tu-extracted"
            -DEXPECT_RC=0
            "-DEXPECT_RE=extracted 1 files \\([0-9]+ bytes\\) in 0 directories"
            "-DEXPECT_FILES=${_fx}/tu-extracted/Default.xexp"
            -P "${_run_expect}")
        set_tests_properties(stfs_extract_all_synthetic PROPERTIES FIXTURES_REQUIRED validate_inputs)
    endif()
endif()
