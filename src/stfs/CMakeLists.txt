# ttt2_stfs - Xbox 360 STFS (LIVE/PIRS/CON) package reader, used for title updates.

add_library(ttt2_stfs STATIC
    stfs.cpp
)
add_library(ttt2::stfs ALIAS ttt2_stfs)

target_include_directories(ttt2_stfs PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(ttt2_stfs PUBLIC ttt2::core)
ttt2_apply_warnings(ttt2_stfs)
