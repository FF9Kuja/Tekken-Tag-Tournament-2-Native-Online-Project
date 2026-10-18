# ttt2_core - endian helpers, bounds-checked reading, hashing, file helpers.
# No dependencies beyond the C++ standard library.

add_library(ttt2_core STATIC
    sha1.cpp
    sha256.cpp
    hex.cpp
    file_io.cpp
)
add_library(ttt2::core ALIAS ttt2_core)

target_include_directories(ttt2_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
)
target_compile_features(ttt2_core PUBLIC cxx_std_20)
ttt2_apply_warnings(ttt2_core)
