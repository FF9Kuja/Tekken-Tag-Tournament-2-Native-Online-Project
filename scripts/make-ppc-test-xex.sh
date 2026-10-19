#!/bin/sh
# Compiles a C test program for 32-bit big-endian PowerPC with Clang, links it with
# recomp/tests/ppc/xex_layout.ld and wraps it into a minimal XEX2 image (scripts/ppc_elf_to_xex.py).
# No game content is involved: this produces recompiler smoke-test inputs from our own sources.
#   scripts/make-ppc-test-xex.sh <source.c> <output.xex> [extra clang flags...]
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:?C source}"
out="${2:?output .xex}"
shift 2
cc="${CC:-clang}"
ld="${LD:-ld.lld}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$cc" --target=powerpc-unknown-linux-gnu -m32 -O1 -ffreestanding -nostdlib -fno-pic \
    -fno-asynchronous-unwind-tables "$@" -c "$src" -o "$tmp/prog.o"
"$ld" -m elf32ppc -static -nostdlib -T "$root/recomp/tests/ppc/xex_layout.ld" -o "$tmp/prog.elf" "$tmp/prog.o"
python3 -I "$root/scripts/ppc_elf_to_xex.py" "$tmp/prog.elf" "$out"
