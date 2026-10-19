# ttt2-validate: one command that checks a disc image and a title update together.
add_executable(ttt2-validate main.cpp)
target_link_libraries(ttt2-validate PRIVATE ttt2::xdvdfs ttt2::stfs ttt2::xex ttt2::core)
if(TARGET ttt2_xexpatcher)
    # With the Clang-only patcher available the tool also applies the update in memory and
    # reports the hashes, imports and helper addresses of the patched image.
    target_link_libraries(ttt2-validate PRIVATE ttt2_xexpatcher)
    target_compile_definitions(ttt2-validate PRIVATE TTT2_HAVE_XEXPATCHER=1)
endif()
ttt2_apply_warnings(ttt2-validate)
install(TARGETS ttt2-validate RUNTIME DESTINATION bin)
