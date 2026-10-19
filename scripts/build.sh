#!/bin/sh
# Configure + build + test with a CMake preset (default: linux-clang).
#   scripts/build.sh [preset] [extra cmake configure args...]
set -eu
preset="${1:-linux-clang}"
[ $# -gt 0 ] && shift
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
cmake --preset "$preset" "$@"
cmake --build --preset "$preset" --parallel
ctest --preset "$preset"
