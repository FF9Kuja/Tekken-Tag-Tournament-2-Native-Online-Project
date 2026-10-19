add_executable(ttt2-stfs main.cpp)
target_link_libraries(ttt2-stfs PRIVATE ttt2::stfs ttt2::xex ttt2::core)
ttt2_apply_warnings(ttt2-stfs)
install(TARGETS ttt2-stfs RUNTIME DESTINATION bin)
