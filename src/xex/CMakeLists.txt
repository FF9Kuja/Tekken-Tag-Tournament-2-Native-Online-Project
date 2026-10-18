# ttt2_xex - XEX2 container header parsing and title identification.

add_library(ttt2_xex STATIC
    xex2.cpp
)
add_library(ttt2::xex ALIAS ttt2_xex)

target_include_directories(ttt2_xex PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(ttt2_xex PUBLIC ttt2::core)
ttt2_apply_warnings(ttt2_xex)
