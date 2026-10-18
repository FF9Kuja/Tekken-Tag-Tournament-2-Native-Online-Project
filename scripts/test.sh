#!/bin/sh
# Runs the tests of an already-built preset (default: linux-clang).
set -eu
preset="${1:-linux-clang}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
ctest --preset "$preset"
