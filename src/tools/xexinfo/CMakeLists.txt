add_executable(ttt2-xexinfo main.cpp)
target_link_libraries(ttt2-xexinfo PRIVATE ttt2::xex ttt2::core)
ttt2_apply_warnings(ttt2-xexinfo)
install(TARGETS ttt2-xexinfo RUNTIME DESTINATION bin)
