#!/bin/sh
# Boots the game host (recomp/: ttt2 / ttt2.exe, the real ReXApp with window, runtime and the
# recompiled code) on a license-free test image and checks that it runs the program and exits
# cleanly. The host build (scripts/build-host.ps1 or the SDK validation workflow) packs
# recomp/tests/ppc/imports_ttt2.s as recomp/game/default.xex for this: its entry point calls kernel
# imports (two overridden by recomp/src/kernel_overrides.cpp) and returns, which makes ReXApp quit.
#
#   scripts/host-boot-test.sh <ttt2 executable> <game data root> [timeout seconds]
#                             [--expect-crash | --expect-stall] [--gpu]
#
# Runs without GPU emulation (--gpu_plugin=none) and with SDL's dummy audio driver; on Linux without a
# display it starts an Xvfb of its own. With --gpu the host is left to load the staged Xenos plugin
# (D3D12 on Windows, a software adapter on a GPU-less runner): the normal expectations apply with
# "graphics backend xenos", and a plugin that fails to load or to set up fails the test; the first
# exercise of the graphics path before the owner's machine. Expects exit code 0 within the timeout and these lines in the
# newest log file next to the executable: the executable identification, both override first-call
# lines, the main guest thread's exit and the host's own exit line; a HOST CRASH block fails the test.
# With --expect-crash (image: recomp/tests/ppc/crash_ttt2.s, a program that writes to the null page)
# the expectations invert: exit code 3 (Ttt2App::kExitCodeHostCrash) and a HOST CRASH block, which is
# how the crash path itself is tested. With --expect-stall (image: recomp/tests/ppc/stall_ttt2.s, a
# program that spins forever) the host runs with a 1 s watchdog period until its "possible stall" dump
# appears (at most the timeout, default 12 s), then the window is closed the way a player closes it
# (SIGTERM, which SDL turns into a close request; WM_CLOSE through taskkill on Windows): the log must
# hold the dump with per-thread lines and the kernel stub report the host writes on close, and the
# host must exit with code 0. That is how the stall detector and the close path are tested. No game
# content is involved.
set -eu
exe="${1:?ttt2 executable}"
game="${2:?game data root}"
limit="${3:-}"
expect_crash=0
expect_stall=0
gpu=0
for a in "$@"; do
    [ "$a" = "--expect-crash" ] && expect_crash=1
    [ "$a" = "--expect-stall" ] && expect_stall=1
    [ "$a" = "--gpu" ] && gpu=1
done
gpu_arg="--gpu_plugin=none"
[ "$gpu" = 0 ] || gpu_arg="--gpu_plugin=xenos"
case "$limit" in ""|--*) limit=60; [ "$expect_stall" = 0 ] || limit=12 ;; esac
[ "$expect_stall" = 0 ] || export TTT2_WATCHDOG_MS=1000
[ -f "$exe" ] || { echo "host-boot-test: $exe not found"; exit 2; }
exe="$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")"
game="$(cd "$game" && pwd)"
[ -f "$game/default.xex" ] || { echo "host-boot-test: $game/default.xex not found"; exit 2; }
logs="$(dirname "$exe")/logs"
stdout_log="$(dirname "$exe")/boot-stdout.log"
rm -rf "$logs"
export SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy
windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac

# A display: Linux without one gets an Xvfb of this script's own (not xvfb-run: the host must be this
# script's direct child so it can be signalled, and the X server must outlive the close request).
xvfb_pid=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Darwin) ;;
    *) if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
           command -v Xvfb > /dev/null 2>&1 || { echo "host-boot-test: no display and no Xvfb (apt install xvfb)"; exit 2; }
           display=":$(( $$ % 900 + 100 ))"
           Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp > /dev/null 2>&1 &
           xvfb_pid=$!
           export DISPLAY="$display"
           sleep 1
       fi ;;
esac
cleanup() { [ -z "$xvfb_pid" ] || kill "$xvfb_pid" 2>/dev/null || true; }
trap cleanup EXIT

rc=0
# --gpu_plugin=none keeps Ttt2App from selecting the staged Xenos plugin (no GPU emulation) unless --gpu.
if [ "$expect_stall" = 1 ]; then
    "$exe" --game_data_root "$game" "$gpu_arg" --log_level info > "$stdout_log" 2>&1 &
    pid=$!
    waited=0
    while [ "$waited" -lt "$limit" ] && ! grep -qs "ttt2: possible stall" "$logs"/*.log 2>/dev/null; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        # Close the window the way the player will.
        if [ "$windows" = 1 ]; then
            winpid="$(ps -W | awk -v p="$pid" '$1 == p { print $4 }')"
            [ -z "$winpid" ] || taskkill //PID "$winpid" > /dev/null 2>&1 || true
        else
            kill -TERM "$pid" 2>/dev/null || true
        fi
        closed=0
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$pid" 2>/dev/null || { closed=1; break; }
            sleep 1
        done
        [ "$closed" = 1 ] || { echo "FAIL boot: the host did not end within 10 s of the close request"; kill -9 "$pid" 2>/dev/null || true; }
    fi
    wait "$pid" 2>/dev/null || rc=$?
else
    timeout "$limit" "$exe" --game_data_root "$game" "$gpu_arg" --log_level info > "$stdout_log" 2>&1 || rc=$?
fi
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
if [ "$expect_stall" = 1 ]; then
    [ "$rc" = 0 ] && echo "ok   boot: exit code 0 after the close request" || { echo "FAIL boot: exit code $rc, expected 0 after the close request"; status=1; }
    check "ttt2: main guest thread created" "the module was launched"
    check "ttt2: watchdog: .* main thread running" "the watchdog saw the main thread running"
    check "ttt2: possible stall: thread census and stub calls unchanged" "the stall detector fired"
    check "ttt2:   .* id [0-9]* .* priority .* wait_result" "the per-thread dump was logged"
    check "ttt2: kernel stub report at window close" "the stub report was written when the window closed"
    check "Window closing" "the host took the close request"
    absent "ttt2: HOST CRASH" "no host crash"
    absent "ttt2: guest program ended" "the program did not end on its own"
elif [ "$expect_crash" = 1 ]; then
    [ "$rc" = 3 ] && echo "ok   boot: exit code 3 after the crash" || { echo "FAIL boot: exit code $rc, expected 3 (crash)"; status=1; }
    check "ttt2: HOST CRASH" "the crash report was logged"
    check "exiting with code 3" "the host ended itself after the report"
    [ "$rc" = 124 ] && echo "FAIL boot: the host kept running after the fault (timeout)" && status=1
else
    [ "$rc" = 0 ] && echo "ok   boot: exit code 0" || { echo "FAIL boot: exit code $rc, expected 0"; status=1; }
    check "ttt2: executable title" "the image was identified"
    if [ "$gpu" = 1 ]; then
        check "ttt2: graphics backend xenos" "the Xenos GPU plugin was selected"
        absent "Failed to load GPU plugin" "the GPU plugin loaded"
        absent "GPU plugin .* not found" "the GPU plugin was staged next to the executable"
        absent "Graphics presentation setup failed" "the graphics presentation was set up"
    else
        check "ttt2: graphics backend none" "no GPU emulation, as requested"
    fi
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
