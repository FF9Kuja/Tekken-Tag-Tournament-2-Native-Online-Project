# ttt2_xdvdfs - Xbox 360 disc image (XDVDFS) reader.

add_library(ttt2_xdvdfs STATIC
    xdvdfs.cpp
)
add_library(ttt2::xdvdfs ALIAS ttt2_xdvdfs)

target_include_directories(ttt2_xdvdfs PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(ttt2_xdvdfs PUBLIC ttt2::core)
ttt2_apply_warnings(ttt2_xdvdfs)
