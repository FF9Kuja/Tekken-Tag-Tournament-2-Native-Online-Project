#!/bin/sh
# Regenerates the ReXGlue SDK's PowerPC instruction tests as Catch2 C++ sources, using the SDK's bundled
# powerpc-none-elf binutils and `rexglue recompile-tests`. No game file is involved: the inputs are the
# SDK's own assembly tests (tests/ppc/asm/*.s). This is the license-free check that the recompiler's
# instruction translation works on this machine.
#
#   scripts/sdk-recompile-tests.sh <rexglue-sdk dir> <rexglue executable> <output dir>
#
# Output: <output dir>/{obj,bin,gen}; gen/ holds ppc_config.h, ppc_test_decls.h, ppc_test_functions.cpp,
# ppc_test_cases.cpp. Building and running them needs the SDK runtime (configure the SDK with
# -DREXGLUE_BUILD_TESTS=ON for the complete pipeline).
set -eu
sdk="${1:?rexglue-sdk directory}"
rexglue="${2:?rexglue executable}"
out="${3:?output directory}"
bu="$sdk/tools/binutils"
asm_dir="$sdk/tests/ppc/asm"
for tool in powerpc-none-elf-as powerpc-none-elf-ld powerpc-none-elf-nm; do
    [ -x "$bu/$tool" ] || { echo "missing $bu/$tool (SDK tools/binutils)"; exit 1; }
done
mkdir -p "$out/obj" "$out/bin" "$out/gen"
n=0
for s in "$asm_dir"/*.s; do
    base="$(basename "$s" .s)"
    "$bu/powerpc-none-elf-as" -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R -o "$out/obj/$base.o" "$s"
    "$bu/powerpc-none-elf-ld" -A powerpc:common32 -melf32ppc -EB -nostdlib --oformat=binary \
        -Ttext=0x82010000 -e 0x82010000 -o "$out/bin/$base.bin" "$out/obj/$base.o"
    "$bu/powerpc-none-elf-nm" --numeric-sort "$out/obj/$base.o" > "$out/bin/$base.map"
    n=$((n + 1))
done
echo "assembled $n test files"
"$rexglue" recompile-tests --bin-dir "$out/bin" --asm-dir "$asm_dir" --output "$out/gen"
echo "generated: $(grep -c TEST_CASE "$out/gen/ppc_test_cases.cpp") Catch2 test cases in $out/gen"
