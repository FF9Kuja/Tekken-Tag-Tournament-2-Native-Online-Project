add_executable(ttt2_tests
    test_main.cpp
    test_endian.cpp
    test_hash.cpp
    test_aes.cpp
    test_xex2.cpp
    test_basefile.cpp
    test_xdvdfs.cpp
)
target_link_libraries(ttt2_tests PRIVATE ttt2::core ttt2::xex ttt2::xdvdfs doctest::doctest)
target_include_directories(ttt2_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
ttt2_apply_warnings(ttt2_tests)

add_test(NAME ttt2_unit_tests COMMAND ttt2_tests --reporters=console --no-intro)

# Smoke test of the tool (only when the tools are built): it must refuse a
# non-XEX file with exit code 1 and print its version with exit code 0.
if(TARGET ttt2-xexinfo)
    add_test(NAME xexinfo_version COMMAND ttt2-xexinfo --version)
    add_test(NAME xexinfo_rejects_non_xex COMMAND ttt2-xexinfo "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
    set_tests_properties(xexinfo_rejects_non_xex PROPERTIES WILL_FAIL TRUE)
endif()
if(TARGET ttt2-iso)
    add_test(NAME iso_version COMMAND ttt2-iso --version)
    add_test(NAME iso_rejects_non_image COMMAND ttt2-iso info "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
    set_tests_properties(iso_rejects_non_image PROPERTIES WILL_FAIL TRUE)
endif()
