#!/bin/sh
# License-free end-to-end smoke test of the ReXGlue recompiler: compiles the PowerPC test programs
# in recomp/tests/ppc, wraps them into XEX2 images (scripts/ppc_elf_to_xex.py), runs
# `rexglue codegen` on each and checks the output. With an SDK source tree given, the generated
# translation units are also syntax-checked against the SDK headers.
#
#   scripts/codegen-smoke-test.sh <rexglue executable> [rexglue-sdk source dir] [work dir]
#
# Exit code 0 when every program recompiles with the expected number of functions and no
# unimplemented-instruction or error markers.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
rexglue="${1:?rexglue executable}"
sdk="${2:-}"
work="${3:-$root/out/codegen-smoke}"
cxx="${CXX:-clang++-20}"
rm -rf "$work"
mkdir -p "$work"
status=0

# program name : expected function count (FUNC symbols in the test source)
for spec in "smoke:6" "jumptable:2"; do
    name="${spec%%:*}"
    expected="${spec##*:}"
    dir="$work/$name"
    mkdir -p "$dir/generated"
    "$root/scripts/make-ppc-test-xex.sh" "$root/recomp/tests/ppc/$name.c" "$dir/$name.xex" > "$dir/pack.log"
    cat > "$dir/${name}_manifest.toml" <<MANIFEST
[project]
name = "$name"
sdk_version = "0.10.0"

[entrypoint]
file_path = "$name.xex"
out_directory_path = "generated/default"
includes = ["${name}_config.toml"]
MANIFEST
    cat > "$dir/${name}_config.toml" <<CONFIG
skip_lr = false
skip_msr = false
ctr_as_local = false
xer_as_local = false
reserved_as_local = false
cr_as_local = false
non_argument_as_local = false
non_volatile_as_local = false

[[invalid_instructions]]
data = 0x00000000
size = 4
CONFIG
    if ! (cd "$dir" && "$rexglue" --log-level warn codegen "${name}_manifest.toml" > codegen.log 2>&1); then
        echo "FAIL $name: codegen exited non-zero (see $dir/codegen.log)"
        tail -20 "$dir/codegen.log"
        status=1
        continue
    fi
    funcs="$(grep -c "DECLARE_REX_FUNC" "$dir/generated/default/${name}_funcs.h" || true)"
    markers="$(cat "$dir"/generated/default/${name}_recomp.*.cpp | grep -c -i "UNIMPLEMENTED\|// ERROR\|__debugbreak" || true)"
    if [ "$funcs" != "$expected" ]; then
        echo "FAIL $name: $funcs functions generated, expected $expected"
        status=1
    elif [ "$markers" != "0" ]; then
        echo "FAIL $name: $markers unimplemented/error markers in the generated code"
        status=1
    else
        echo "ok   $name: $funcs functions, no error markers"
    fi
    if [ -n "$sdk" ] && [ -d "$sdk/include/rex" ]; then
        for tu in "$dir"/generated/default/*.cpp; do
            if "$cxx" -std=c++23 -fsyntax-only -mssse3 -fno-char8_t -DREX_PLATFORM_LINUX=1 -DSPDLOG_COMPILED_LIB \
                -DSPDLOG_FMT_EXTERNAL -DREX_HAS_VULKAN=1 -I"$sdk/include" -I"$sdk/build/linux/include" \
                -I"$sdk/build/sdk-native/include" -I"$sdk" -I"$sdk/thirdparty/fmt/include" -I"$sdk/thirdparty/simde" \
                -I"$sdk/thirdparty/simde/simde" -I"$sdk/thirdparty/spdlog/include" -I"$sdk/thirdparty/disruptorplus/include" \
                -I"$sdk/thirdparty/utfcpp/source" -I"$sdk/thirdparty/xxHash" -isystem "$sdk/thirdparty/tomlplusplus/include" \
                -I"$dir/generated/default" -I"$dir" "$tu" > "$dir/syntax.log" 2>&1; then
                echo "ok   $name: $(basename "$tu") compiles against the SDK headers"
            else
                echo "FAIL $name: $(basename "$tu") does not compile against the SDK headers"
                tail -10 "$dir/syntax.log"
                status=1
            fi
        done
    fi
done
exit $status
