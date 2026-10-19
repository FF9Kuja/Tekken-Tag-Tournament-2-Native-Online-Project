#!/bin/sh
# Boots the game host (recomp/: ttt2 / ttt2.exe, the real ReXApp with window, runtime and the
# recompiled code) on a license-free test image and checks that it runs the program and exits
# cleanly. The host build (scripts/build-host.ps1 or the SDK validation workflow) packs
# recomp/tests/ppc/imports_ttt2.s as recomp/game/default.xex for this: its entry point calls kernel
# imports (two overridden by recomp/src/kernel_overrides.cpp) and returns, which makes ReXApp quit.
#
#   scripts/host-boot-test.sh <ttt2 executable> <game data root> [timeout seconds]
#
# Runs without GPU emulation (--gpu_plugin=none) and with SDL's dummy audio driver; on Linux without a
# display it runs under xvfb-run. Expects exit code 0 within the timeout and these lines in the
# newest log file next to the executable: the executable identification, both override first-call
# lines, the main guest thread's exit and the host's own exit line; a HOST CRASH block fails the test.
# With --expect-crash the expectations invert: exit code 3 (Ttt2App::kExitCodeHostCrash) and a
# HOST CRASH block, which is how the crash path itself is tested. No game content is involved.
set -eu
exe="${1:?ttt2 executable}"
game="${2:?game data root}"
limit="${3:-60}"
expect_crash=0
for a in "$@"; do [ "$a" = "--expect-crash" ] && expect_crash=1; done
[ -f "$exe" ] || { echo "host-boot-test: $exe not found"; exit 2; }
exe="$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")"
game="$(cd "$game" && pwd)"
[ -f "$game/default.xex" ] || { echo "host-boot-test: $game/default.xex not found"; exit 2; }
logs="$(dirname "$exe")/logs"
rm -rf "$logs"
export SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy
runner=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Darwin) ;;
    *) if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
           command -v xvfb-run > /dev/null 2>&1 || { echo "host-boot-test: no display and no xvfb-run (apt install xvfb)"; exit 2; }
           runner="xvfb-run -a"
       fi ;;
esac
rc=0
# --gpu_plugin=none keeps Ttt2App from selecting the staged Xenos plugin: no GPU emulation here.
# shellcheck disable=SC2086
timeout "$limit" $runner "$exe" --game_data_root "$game" --gpu_plugin=none --log_level info > "$(dirname "$exe")/boot-stdout.log" 2>&1 || rc=$?
log="$(ls -t "$logs"/*.log 2>/dev/null | head -1)"
echo "host-boot-test: exit code $rc, log ${log:-<none>}"
[ -n "$log" ] || { echo "FAIL host-boot-test: no log file was written to $logs"; exit 1; }
status=0
check() {  # <pattern> <description>
    if grep -q "$1" "$log"; then echo "ok   boot: $2"; else echo "FAIL boot: $2 (no '$1' in the log)"; status=1; fi
}
absent() {
    if grep -q "$1" "$log"; then echo "FAIL boot: $2 ('$1' found in the log)"; status=1; else echo "ok   boot: $2"; fi
}
if [ "$expect_crash" = 1 ]; then
    [ "$rc" = 3 ] && echo "ok   boot: exit code 3 after the crash" || { echo "FAIL boot: exit code $rc, expected 3 (crash)"; status=1; }
    check "ttt2: HOST CRASH" "the crash report was logged"
    check "exiting with code 3" "the host ended itself after the report"
    [ "$rc" = 124 ] && echo "FAIL boot: the host kept running after the fault (timeout)" && status=1
else
    [ "$rc" = 0 ] && echo "ok   boot: exit code 0" || { echo "FAIL boot: exit code $rc, expected 0"; status=1; }
    check "ttt2: executable title" "the image was identified"
    check "ttt2: graphics backend none" "no GPU emulation, as requested"
    check "ttt2: main guest thread created" "the module was launched"
    check "kernel override XamShowFriendsUI -> ERROR_SUCCESS" "XamShowFriendsUI reached the host override"
    check "kernel override XamUserCreateStatsEnumerator -> ERROR_NO_SUCH_USER" "XamUserCreateStatsEnumerator reached the host override"
    check "ttt2: main guest thread exited" "the program returned from its entry point"
    check "ttt2: guest program ended; exiting" "the host ended after the guest exit"
    absent "ttt2: HOST CRASH" "no host crash"
    absent "STUB" "no runtime stub was reached"
fi
[ "$status" = 0 ] && echo "host-boot-test: PASS" || { echo "host-boot-test: FAIL (see $log)"; tail -30 "$log"; }
exit $status
