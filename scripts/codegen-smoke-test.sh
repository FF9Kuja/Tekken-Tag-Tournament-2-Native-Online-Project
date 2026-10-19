#!/bin/sh
# License-free end-to-end smoke test of the ReXGlue recompiler.
#
# Builds the PowerPC test programs in recomp/tests/ppc (C with Clang, assembly with the SDK's
# bundled binutils), wraps each into an XEX2 image (scripts/make-ppc-test-xex.sh), runs
# `rexglue codegen` on it and checks the result with recomp/tools/codegen_report.py: expected
# function count, expected number of recognised jump tables, expected number of unimplemented
# instruction sites, every listed TTT2 instruction translated, and no analysis errors. With an SDK
# source tree given, the generated translation units are also syntax-checked against the SDK
# headers. Each case leaves REPORT.md / report.json / codegen.log in the work directory.
#
#   scripts/codegen-smoke-test.sh <rexglue executable> [rexglue-sdk source dir] [work dir] [--exec]
#
# --exec additionally builds recomp/tests/host (the ReXGlue runtime in tool mode plus the generated
# code; a long build, reused across runs when TTT2_HOST_BUILD_DIR points at an existing one) and
# RUNS the switch-table program natively, checking the 12 values it stores: once with the SDK's own
# jump-table detection (expected to fail: SDK 0.10.0 switches on the wrong register for several
# shapes, measured in session 3) and once with the finder's [[switch_tables]] (must pass).
#
# Exit code 0 when every case passes. No game content is involved.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
exec_mode=0
positional=""
for a in "$@"; do
    if [ "$a" = "--exec" ]; then exec_mode=1; else positional="$positional $a"; fi
done
# shellcheck disable=SC2086
set -- $positional
rexglue="${1:?rexglue executable}"
sdk="${2:-}"
work="${3:-$root/out/codegen-smoke}"
cxx="${CXX:-clang++-20}"
cc="${CC:-clang-20}"
report="$root/recomp/tools/codegen_report.py"
# Cases run inside their own directories, so every path must be absolute.
[ -x "$rexglue" ] || { echo "rexglue not found or not executable: $rexglue"; exit 2; }
rexglue="$(cd "$(dirname "$rexglue")" && pwd)/$(basename "$rexglue")"
[ -z "$sdk" ] || sdk="$(cd "$sdk" && pwd)"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
rm -rf "$work"/smoke "$work"/jumptable "$work"/switch_ttt2 "$work"/insn_ttt2
status=0

# run_case <name> <source> <expected functions> <expected jump tables> <expected unimplemented sites> [extra report options]
run_case() {
    name="$1"; src="$2"; funcs="$3"; switches="$4"; unimpl="$5"; extra="${6:-}"
    dir="$work/$name"
    mkdir -p "$dir/generated"
    # The packer gets this script's compiler, so a versioned toolchain (clang-20 + ld.lld-20, as the
    # SDK workflow installs) is used consistently instead of whatever unversioned clang is on the PATH.
    if ! CC="$cc" "$root/scripts/make-ppc-test-xex.sh" "$src" "$dir/$name.xex" > "$dir/pack.log" 2>&1; then
        echo "FAIL $name: could not build the test image (see $dir/pack.log)"
        tail -5 "$dir/pack.log"
        status=1
        return
    fi
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
CONFIG
    if ! (cd "$dir" && "$rexglue" --log-level debug codegen "${name}_manifest.toml" > codegen.log 2>&1); then
        echo "FAIL $name: codegen exited non-zero (see $dir/codegen.log)"
        grep -i "error" "$dir/codegen.log" | tail -20
        status=1
        return
    fi
    # shellcheck disable=SC2086
    if python3 -I "$report" "$dir/generated/default" --log "$dir/codegen.log" --project "$name" \
            --md "$dir/REPORT.md" --json "$dir/report.json" --quiet \
            --expect-functions "$funcs" --expect-switches "$switches" --expect-unimplemented "$unimpl" \
            --fail-on-errors $extra; then
        echo "ok   $name: $funcs functions, $switches jump tables, $unimpl unimplemented sites, no errors"
    else
        echo "FAIL $name: report checks failed (see $dir/REPORT.md)"
        grep "FAIL" "$dir/REPORT.md" || true
        status=1
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
}

# C programs (Clang): function counts are the FUNC symbols. Clang lowers both switches to value
# tables, not bctr dispatches (verified with -S), so the jump-table coverage is the assembly case.
run_case smoke     "$root/recomp/tests/ppc/smoke.c"     6 0 0
run_case jumptable "$root/recomp/tests/ppc/jumptable.c" 2 0 0

# Assembly programs need the SDK's PowerPC binutils (VMX128); skipped without an SDK checkout.
bu="${TTT2_PPC_BINUTILS:-${sdk:-$root/third_party/rexglue-sdk/src}/tools/binutils}"
if [ -x "$bu/powerpc-none-elf-as" ]; then
    export TTT2_PPC_BINUTILS="$bu"
    # Pass 1, SDK detection alone: 11 .pdata functions + one leaf discovered through its call; the SDK
    # recognises 7 of the 9 dispatches (it misses the byte- and halfword-offset tables; measured with
    # SDK 0.10.0 and documented in docs/RESEARCH.md). None at the vtable call.
    run_case switch_ttt2 "$root/recomp/tests/ppc/switch_ttt2.s" 12 7 0
    dir="$work/switch_ttt2"
    # Keep the SDK-only generation for the execution check before pass 2 overwrites it.
    rm -rf "$dir/generated-sdk-only"
    cp -r "$dir/generated" "$dir/generated-sdk-only"
    # Pass 2: our finder (recomp/tools/find_switch_tables.py) must see all 9 and, included as manual
    # [[switch_tables]] entries, make the codegen emit 9 switches. Manual entries take precedence
    # over the SDK's detection, so this is also the path the real game's config uses.
    if python3 -I "$root/recomp/tools/find_switch_tables.py" "$dir/switch_ttt2.xex" \
            --toml "$dir/switch_ttt2_tables.toml" --json "$dir/switch_tables.json" > "$dir/finder.log" 2>&1 \
        && [ "$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))['found'])" "$dir/switch_tables.json")" = "9" ]; then
        echo "ok   switch_ttt2: finder found 9 switch tables"
    else
        echo "FAIL switch_ttt2: finder did not find 9 switch tables (see $dir/finder.log)"
        cat "$dir/finder.log"
        status=1
    fi
    printf 'includes = ["switch_ttt2_tables.toml"]\n' >> "$dir/switch_ttt2_config.toml"
    if (cd "$dir" && "$rexglue" --log-level debug codegen --ignore-stamp switch_ttt2_manifest.toml > codegen2.log 2>&1) \
        && python3 -I "$report" "$dir/generated/default" --log "$dir/codegen2.log" --project switch_ttt2 \
            --md "$dir/REPORT2.md" --json "$dir/report2.json" --quiet \
            --expect-functions 12 --expect-switches 9 --expect-unimplemented 0 --fail-on-errors; then
        echo "ok   switch_ttt2: with the finder's tables, 9 jump tables, no errors"
    else
        echo "FAIL switch_ttt2: pass 2 with manual switch tables (see $dir/REPORT2.md, $dir/codegen2.log)"
        grep "FAIL" "$dir/REPORT2.md" 2>/dev/null || tail -5 "$dir/codegen2.log"
        status=1
    fi

    # every instruction of recomp/ttt2_instructions.txt must appear translated, except macchwu/maclhwu:
    # SDK 0.10.0 decodes those opcode-4 words as VMX128 (vsldoi128), so they neither translate nor trap.
    run_case insn_ttt2 "$root/recomp/tests/ppc/insn_ttt2.s" 8 0 0 \
        "--expect-mnemonics $root/recomp/ttt2_instructions.txt --absent-ok macchwu,maclhwu"

    if [ "$exec_mode" = 1 ] && [ -n "$sdk" ]; then
        # Native execution through the ReXGlue runtime (recomp/tests/host). The program stores 12
        # words at `results` (address from the packer's symbol map) and returns their sum.
        dir="$work/switch_ttt2"
        results="$(awk '$4 == "results" {print $1}' "$dir/switch_ttt2.xex.syms")"
        expected="20,102,201,301,401,500,600,701,900,800,0,9"
        hostbuild="${TTT2_HOST_BUILD_DIR:-$work/host-build}"
        run_host() {  # <generated dir>: builds the harness for it and runs the program
            cmake -S "$root/recomp/tests/host" -B "$hostbuild" -G Ninja \
                -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_FLAGS=-mssse3 -DCMAKE_CXX_FLAGS=-mssse3 -DSDL_X11_XSCRNSAVER=OFF \
                -DREXSDK_DIR="$sdk" -DTTT2_GUEST_PROJECT=switch_ttt2 -DTTT2_GUEST_GENERATED="$1" > "$hostbuild.configure.log" 2>&1 \
                && cmake --build "$hostbuild" --target headless_host --parallel > "$hostbuild.build.log" 2>&1 || {
                echo "FAIL exec: building the headless host failed (see $hostbuild.build.log)"; tail -20 "$hostbuild.build.log"; return 2; }
            host="$(find "$sdk/out" \( -name headless_host -o -name headless_host.exe \) -type f 2>/dev/null | head -1)"
            [ -n "$host" ] || { echo "FAIL exec: headless_host not found under $sdk/out"; return 2; }
            # --max-stub-calls 0: the program must not reach any kernel export the runtime only stubs.
            (cd "$dir" && timeout 120 "$host" --game-root "$dir" --xex switch_ttt2.xex --results "$results:12" \
                --expect "$expected" --max-stub-calls 0 --log-level warn)
        }
        # Harness exit codes: 0 all values as expected and no stub calls, 1 mismatch or stub calls,
        # 2 build/setup failure, 3 the guest never finished; a trap in the guest ends the process with
        # a signal (>= 128).
        rc=0
        run_host "$dir/generated-sdk-only/default" > "$dir/exec-sdk-only.log" 2>&1 || rc=$?
        case "$rc" in
            0) echo "FAIL exec: the SDK-only switch tables ran correctly, which contradicts the measured SDK 0.10.0 behaviour (see $dir/exec-sdk-only.log)"; status=1 ;;
            2) echo "FAIL exec: could not build or start the headless host for the SDK-only code (see $dir/exec-sdk-only.log)"; tail -20 "$dir/exec-sdk-only.log"; status=1 ;;
            *) echo "ok   exec: SDK-only jump tables do not run correctly (exit $rc, as measured; see $dir/exec-sdk-only.log)" ;;
        esac
        rc=0
        run_host "$dir/generated/default" > "$dir/exec-finder.log" 2>&1 || rc=$?
        if [ "$rc" = 0 ] && grep -q "headless_host: PASS" "$dir/exec-finder.log" \
            && grep -q "headless_host: kernel stubs: 0 calls" "$dir/exec-finder.log"; then
            echo "ok   exec: native run with the finder's switch tables produced all 12 expected values, no kernel stubs reached"
        else
            echo "FAIL exec: native run with the finder's switch tables (exit $rc, see $dir/exec-finder.log)"
            tail -20 "$dir/exec-finder.log"
            status=1
        fi
    fi
else
    echo "skip switch_ttt2 / insn_ttt2: PowerPC binutils not found at $bu"
fi
exit $status
