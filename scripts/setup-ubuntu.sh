#!/bin/sh
# Installs the tools needed to build this repository on Ubuntu 24.04 / Debian.
# (The ReXGlue SDK itself needs more: see docs/DEVELOPMENT.md.)
set -eu
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential clang lld cmake ninja-build python3 git
echo "done. Build with: scripts/build.sh linux-clang"
