#!/bin/sh
# Boots the game host (recomp/: ttt2 / ttt2.exe, the real ReXApp with window, runtime and the
# recompiled code) on a license-free test image and checks that it runs the program and exits
# cleanly. The host build (scripts/build-host.ps1 or the SDK validation workflow) packs
# recomp/tests/ppc/imports_ttt2.s as recomp/game/default.xex for this: its entry point calls kernel
# imports (two overridden by recomp/src/kernel_overrides.cpp) and returns, which makes ReXApp quit.
#
#   scripts/host-boot-test.sh <ttt2 executable> <game data root> [timeout seconds]
#                             [--expect-crash | --expect-stall | --expect-fatal] [--gpu] [--no-imports]
#                             [--expect-waits] [--expect-io] [--expect-gpu-frames]
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
# host must exit with code 0. That is how the stall detector and the close path are tested. With
# --expect-fatal (image: recomp/tests/ppc/tailcall_ttt2.s with only its codegen-time boundaries resolved)
# the program calls a function no recompiled code starts at: the runtime's REX_FATAL line and the host's
# own abort line must be logged and the host must exit with code 4 (Ttt2App::kExitCodeFatal) instead of
# hanging in the C runtime's abort dialog. --no-imports drops the two kernel-override checks of the
# normal expectations, for programs other than imports_ttt2.s. --expect-waits (with --expect-stall; image:
# recomp/tests/ppc/wait_ttt2.s, two threads deadlocked on a critical section and an event next to a third
# that spins on its own event) also checks what the host's boot monitor says about the stall: the threads'
# creation with their start addresses, the main thread's wait on the critical section naming the worker as
# its owner, the worker's wait on the unsignaled event, the spinner flagged as busy with its busiest call,
# the guest call sites and stacks, and the thread dump the host writes on window close. --expect-io (image: recomp/tests/ppc/io_ttt2.s; writes the 16-byte io_data.bin it reads into
# the game root) checks the host's file-system trace: the sector query answered as unsupported, the
# bare device \Device\Image failing to open with the runtime's "ResolvePath(\Device)" line, answered as a
# missing name the way the console answers it (STATUS_OBJECT_NAME_NOT_FOUND) with the host's explanation,
# the file opened, and the watchdog's activity counters. --expect-gpu-frames (image:
# recomp/tests/ppc/gpu_ttt2.s, three frames of GPU commands handed to VdSwap) checks the host's frame
# monitor: each frame's draws, resolves, clears, indirect buffers and skipped packets, the front buffer, and
# the frame summary in the activity line. Every normal run also checks that the host keeps the kernel clock
# (KeTimeStampBundle) current. No game content is involved.
set -eu
exe="${1:?ttt2 executable}"
game="${2:?game data root}"
limit="${3:-}"
expect_crash=0
expect_stall=0
expect_fatal=0
gpu=0
imports=1
expect_waits=0
expect_io=0
expect_gpu_frames=0
for a in "$@"; do
    [ "$a" = "--expect-crash" ] && expect_crash=1
    [ "$a" = "--expect-stall" ] && expect_stall=1
    [ "$a" = "--expect-fatal" ] && expect_fatal=1
    [ "$a" = "--gpu" ] && gpu=1
    [ "$a" = "--no-imports" ] && imports=0
    [ "$a" = "--expect-waits" ] && expect_waits=1
    [ "$a" = "--expect-io" ] && expect_io=1
    [ "$a" = "--expect-gpu-frames" ] && expect_gpu_frames=1
done
gpu_arg="--gpu_plugin=none"
[ "$gpu" = 0 ] || gpu_arg="--gpu_plugin=xenos"
case "$limit" in ""|--*) limit=60; [ "$expect_stall" = 0 ] || limit=12 ;; esac
[ "$expect_stall" = 0 ] || export TTT2_WATCHDOG_MS=1000
[ -f "$exe" ] || { echo "host-boot-test: $exe not found"; exit 2; }
exe="$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")"
game="$(cd "$game" && pwd)"
[ -f "$game/default.xex" ] || { echo "host-boot-test: $game/default.xex not found"; exit 2; }
# io_ttt2.s reads this file of the game root (16 bytes, the first word "TTT2").
[ "$expect_io" = 0 ] || printf 'TTT2-IO-TEST-OK!' > "$game/io_data.bin"
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
    check "ttt2: possible stall: thread census, stub calls and boot progress .* unchanged" "the stall detector fired"
    check "ttt2:   .* id [0-9]* .* priority .* wait_result" "the per-thread dump was logged"
    check "ttt2: kernel stub report at window close" "the stub report was written when the window closed"
    check "Window closing" "the host took the close request"
    absent "ttt2: HOST CRASH" "no host crash"
    absent "ttt2: guest program ended" "the program did not end on its own"
    if [ "$expect_waits" = 1 ]; then
        check "ttt2: thread created: id [0-9]*, start 0x[0-9A-F]* (sub_[0-9A-F]*)" "the worker's creation was traced with its start address"
        check "wait: RtlEnterCriticalSection on critical section 0x[0-9A-F]* (owned by thread id [0-9]*, recursion 1), no timeout, for at least" \
            "the main thread's wait names the critical section and its owner"
        check "wait: KeWaitForSingleObject on 0x[0-9A-F]* (synchronization event, not signaled), no timeout, for at least" \
            "the worker's wait names the unsignaled event"
        check "called from 0x[0-9A-F]* (sub_[0-9A-F]*+0x[0-9A-F]*)" "the waits name their guest call sites"
        check "guest stack: 0x[0-9A-F]* (sub_" "the guest call stacks were read"
        check "kernel calls: 0 in the last tick, none for at least" "the quiet threads were named as such"
        check "kernel calls: [0-9]* in the last tick (NtSignalAndWaitForSingleObjectEx [0-9]*" "the spinner's busiest call was named"
        check "busy: [0-9]* traced kernel calls a second over the last [0-9.]* s (mostly NtSignalAndWaitForSingleObjectEx), last from 0x[0-9A-F]* (sub_" \
            "the spinning thread was flagged as busy"
        check "ttt2: thread state at window close:" "the thread dump was written on window close"
    fi
elif [ "$expect_fatal" = 1 ]; then
    [ "$rc" = 4 ] && echo "ok   boot: exit code 4 after the runtime aborted" || { echo "FAIL boot: exit code $rc, expected 4 (runtime abort)"; status=1; }
    check "\[FATAL\] Call to invalid or unregistered function at guest address" "the runtime's trap named the called address"
    check "ttt2: the runtime aborted" "the host logged the abort and ended itself"
    absent "ttt2: HOST CRASH" "no host crash"
    [ "$rc" = 124 ] && echo "FAIL boot: the host kept running after the abort (timeout)" && status=1
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
    if [ "$imports" = 1 ]; then
        check "kernel override XamShowFriendsUI -> ERROR_SUCCESS" "XamShowFriendsUI reached the host override"
        check "kernel override XamUserCreateStatsEnumerator -> ERROR_NO_SUCH_USER" "XamUserCreateStatsEnumerator reached the host override"
    fi
    check "ttt2: main guest thread exited" "the program returned from its entry point"
    check "ttt2: guest program ended; exiting" "the host ended after the guest exit"
    check "ttt2: [0-9]* kernel exports traced by the host" "the host's export wrappers found the runtime's originals"
    check "ttt2: kernel clock: KeTimeStampBundle at 0x[0-9A-F]*: the host refreshes InterruptTime and SystemTime" \
        "the host keeps the kernel clock current"
    check "ttt2: activity: .*; kernel clock [0-9.]* s (tick count [0-9.]* s)" "the activity line reports the kernel clock"
    if [ "$expect_io" = 1 ]; then
        check "ttt2: file opened: 'game:.io_data.bin'" "the file open was traced"
        check "ttt2: sector query: FileSectorInformation for .*io_data.bin' answered as unsupported" "the sector query was answered as unsupported"
        check "ResolvePath(.Device) failed - device not found" "the bare device open produced the runtime's line from the owner's log"
        check "ttt2: the game opened '.Device.Image', a device itself" "the host explained the device open"
        check "ttt2: missing name: '.Device.Image' (NtCreateFile) answered as 0xC0000034 (STATUS_OBJECT_NAME_NOT_FOUND)" \
            "the missing device was answered as the console answers a missing name"
        check "ttt2: file open failed: '.Device.Image' -> 0xC0000034 (STATUS_OBJECT_NAME_NOT_FOUND)" "the failed open was traced with its status and caller"
        check "ttt2: file attributes missing: 'game:.missing.bin' -> 0xC0000034" "the missing file's attribute query was traced"
        check "ttt2: file open failed: 'game:.missing.bin' -> 0xC0000034 (STATUS_OBJECT_NAME_NOT_FOUND) (NtOpenFile" \
            "the missing file's NtOpenFile was answered as a missing name"
        check "ttt2: activity: files 1 opened, 2 failed (1 device paths); read 0.0 MB in 1 reads (0 failed); sector queries 1; missing names 3;" \
            "the watchdog's activity line counted them"
    fi
    if [ "$expect_gpu_frames" = 1 ]; then
        check "ttt2: gpu: ring buffer at 0x[0-9A-F]*, size 2^9" "the ring buffer was registered"
        check "ttt2: gpu: front buffer 16x16 k_8_8_8_8 at 0x[0-9A-F]* (frame 1)" "the front buffer was identified"
        check "ttt2: gpu: frame 1: 3 draws, 1 resolve (1 clearing), 1 render target change, 1 indirect buffer, 1 packet the GPU emulation skips (opcode 0x25), 10 packets (26 words of commands)" \
            "frame 1's draws, resolve, clear, indirect buffer and skipped packet were counted"
        check "ttt2: gpu: first frame with draws: frame 1:" "the first frame with draws was reported"
        check "ttt2: gpu: frame 1 holds packets the GPU emulation does not execute" "the packet the GPU emulation skips was reported"
        check "ttt2: gpu: frame 2: 1 draw with EDRAM mode off, 2 packets (4 words of commands)" "frame 2's non-rendering draw was told apart"
        check "ttt2: gpu: frame 3: 2 draws, 4 packets (872 words of commands)" "frame 3 was read across the end of the ring"
        check "frames presented 3 (2 with draws, 1 with resolves; draws 5, resolves 1, clears 1, at most 3 draws in a frame, 1 packet the GPU emulation skips; last frame: 2 draws, 4 packets; front buffer 16x16)" \
            "the activity line summarised the frames"
    fi
    absent "ttt2: HOST CRASH" "no host crash"
    absent "STUB" "no runtime stub was reached"
fi
[ "$status" = 0 ] && echo "host-boot-test: PASS" || { echo "host-boot-test: FAIL (see $log)"; tail -30 "$log"; }
exit $status
