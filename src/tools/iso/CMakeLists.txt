add_executable(ttt2-iso main.cpp)
target_link_libraries(ttt2-iso PRIVATE ttt2::xdvdfs ttt2::xex ttt2::core)
ttt2_apply_warnings(ttt2-iso)
install(TARGETS ttt2-iso RUNTIME DESTINATION bin)
