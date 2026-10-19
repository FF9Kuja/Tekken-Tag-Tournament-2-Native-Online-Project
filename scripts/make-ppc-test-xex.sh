#!/bin/sh
# Builds a license-free PowerPC test program and wraps it into a minimal XEX2 image
# (scripts/ppc_elf_to_xex.py) for recompiler smoke tests. No game content is involved.
#
#   scripts/make-ppc-test-xex.sh <source.c|source.s> <output.xex> [extra compiler flags...]
#
# C sources are compiled with Clang for 32-bit big-endian PowerPC and linked with LLD (ld.lld, a
# versioned ld.lld-NN, or the clang driver's own lld; CC/LD override the tools).
# Assembly sources (.s, GNU syntax with -mregnames, VMX128 allowed) are assembled and linked
# with the PowerPC binutils bundled in the ReXGlue SDK checkout (tools/binutils), because
# Clang's integrated assembler knows neither the VMX128 extension nor the Xbox 360 mnemonics.
# Point TTT2_PPC_BINUTILS at another binutils directory to override. A symbol map is written next
# to the image as <output.xex>.syms (address, size, kind, name per line).
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:?C or assembly source}"
out="${2:?output .xex}"
shift 2
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
case "$src" in
    *.s|*.S)
        bu="${TTT2_PPC_BINUTILS:-$root/third_party/rexglue-sdk/src/tools/binutils}"
        [ -x "$bu/powerpc-none-elf-as" ] || { echo "PowerPC assembler not found in $bu (scripts/fetch-rexglue-sdk.sh)"; exit 1; }
        "$bu/powerpc-none-elf-as" -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 "$@" -o "$tmp/prog.o" "$src"
        "$bu/powerpc-none-elf-ld" -melf32ppc -EB -nostdlib -T "$root/recomp/tests/ppc/xex_layout.ld" -o "$tmp/prog.elf" "$tmp/prog.o"
        ;;
    *)
        cc="${CC:-clang}"
        case "$cc" in *gcc*|*g++*) cc=clang ;; esac   # a GCC build of the tools still packs with Clang
        command -v "$cc" > /dev/null 2>&1 || { echo "$cc not found: install clang (and lld)"; exit 1; }
        "$cc" --target=powerpc-unknown-linux-gnu -m32 -O1 -ffreestanding -nostdlib -fno-pic \
            -fno-asynchronous-unwind-tables "$@" -c "$src" -o "$tmp/prog.o"
        # Linker: $LD, else ld.lld, else the ld.lld-<major> of this clang (Ubuntu's lld-NN packages
        # install no unversioned name), else let the clang driver find its own lld (-fuse-ld=lld).
        ld="${LD:-}"
        if [ -z "$ld" ]; then
            if command -v ld.lld > /dev/null 2>&1; then
                ld=ld.lld
            else
                major="$("$cc" -dumpversion 2>/dev/null | cut -d. -f1)"
                if [ -n "$major" ] && command -v "ld.lld-$major" > /dev/null 2>&1; then ld="ld.lld-$major"; fi
            fi
        fi
        if [ -n "$ld" ]; then
            command -v "$ld" > /dev/null 2>&1 || { echo "$ld not found: install lld"; exit 1; }
            "$ld" -m elf32ppc -static -nostdlib -T "$root/recomp/tests/ppc/xex_layout.ld" -o "$tmp/prog.elf" "$tmp/prog.o"
        else
            "$cc" --target=powerpc-unknown-linux-gnu -m32 -fuse-ld=lld -static -nostdlib -nostartfiles \
                -Wl,-T,"$root/recomp/tests/ppc/xex_layout.ld" -o "$tmp/prog.elf" "$tmp/prog.o" \
                || { echo "no lld linker found (ld.lld, ld.lld-<version>, or clang -fuse-ld=lld): install lld"; exit 1; }
        fi
        ;;
esac
python3 -I "$root/scripts/ppc_elf_to_xex.py" "$tmp/prog.elf" "$out" --symbols "$out.syms"
