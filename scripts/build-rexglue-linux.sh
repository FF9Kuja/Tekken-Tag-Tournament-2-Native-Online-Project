#!/bin/sh
# Builds the ReXGlue codegen tool (and optionally its PPC instruction tests) on Linux.
#   scripts/build-rexglue-linux.sh [sdk dir] [--with-tests]
# Requirements measured on Ubuntu 24.04: Clang 20, -mssse3, the SDL3/GTK3/XCB development packages
# listed in docs/DEVELOPMENT.md, nasm for FFmpeg.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${1:-$root/third_party/rexglue-sdk/src}"
with_tests=OFF
for a in "$@"; do [ "$a" = "--with-tests" ] && with_tests=ON; done
cc="${CC:-clang-20}"; cxx="${CXX:-clang++-20}"
command -v "$cxx" >/dev/null || { echo "$cxx not found (the SDK needs Clang 20+ on Linux)"; exit 1; }
cmake -S "$sdk" -B "$sdk/build/linux" -G Ninja \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS=-mssse3 -DCMAKE_CXX_FLAGS=-mssse3 \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF -DSDL_X11_XSCRNSAVER=OFF \
    -DREXGLUE_BUILD_TESTS="$with_tests"
cmake --build "$sdk/build/linux" --target rexglue --parallel
if [ "$with_tests" = ON ]; then
    cmake --build "$sdk/build/linux" --target ppc_tests --parallel
    "$sdk/out/linux-amd64/ppc_tests"
fi
echo "rexglue: $sdk/out/linux-amd64/rexglue"
