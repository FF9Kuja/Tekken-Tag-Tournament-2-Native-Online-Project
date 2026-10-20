#!/usr/bin/env python3
"""Triage of a ttt2 host boot log: what happened, where it stopped, which kernel stubs the game
reached and what to do next.

The host (recomp/src/ttt2_app.cpp) logs its diagnostics as `ttt2: ...` lines next to the ReXGlue
runtime's own messages. This tool reads one log session and writes a short report so a boot attempt on
the owner's machine turns into an action list without reading megabytes by hand.

    python3 recomp/tools/triage_boot_log.py <log file | logs directory> [--md out.md] [--json out.json]
                                            [--ttt2web-list recomp/tools/ttt2web_kernel_functions.txt]
                                            [--quiet]

A directory means the newest log session: `ttt2_001.log` plus its rotated parts `ttt2_001.N.log`
(spdlog rotation, higher N is older), read oldest first. The report holds names, addresses and counts
taken from the log; no game data is involved. Exit code 0 (the report is the result), 2 on bad input.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys
from datetime import datetime

RE_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[(\w+)\] \[(\w+)\] (?:\[t(\d+)\] )?(.*)$")
RE_TITLE = re.compile(r"ttt2: executable title ([0-9A-Fa-f]{8}) version (\S+) \(base (\S+)\), media ([0-9A-Fa-f]{8})")
RE_BACKEND = re.compile(r"ttt2: graphics backend (.+)")
RE_ROOT = re.compile(r"ttt2: (game data root|update data root|user data root|cache root)\s+(.*)")
RE_IMPORTS = re.compile(r"ttt2: imports (\d+) entries from (\w+)")
RE_MAIN_CREATED = re.compile(r"ttt2: main guest thread created \(id (\d+)\)")
RE_MAIN_EXITED = re.compile(r"ttt2: main guest thread exited \(id (\d+)\)")
RE_ENDED = re.compile(r"ttt2: guest program ended; exiting with code (\d+)")
RE_OVERRIDE = re.compile(r"ttt2: kernel override (\w+) -> (.*) \(first call\)")
RE_WATCHDOG = re.compile(r"ttt2: watchdog: (\d+) guest threads \((\d+) running\); main thread (running|not running): (.*); kernel stubs: (.*)")
# Both forms: "thread census and stub calls unchanged" (hosts before the boot monitor) and "thread census,
# stub calls and boot progress (files, reads, frames) unchanged".
RE_STALL = re.compile(r"ttt2: possible stall: thread census.*? unchanged for (\d+) ms")
RE_STILL_STALLED = re.compile(r"ttt2: still stalled after (\d+) ms")
RE_DUMP_HEADER = re.compile(r"ttt2: thread state at (.+):$")
RE_STALL_THREAD = re.compile(r"ttt2:\s+(\S.*?)\s+id (\d+)\s+(running|stopped) priority (-?\d+) suspend (\d+) wait_result (0x[0-9a-fA-F]+)")
# The boot monitor's lines (recomp/src/boot_monitor.cpp and the export wrappers).
RE_THREAD_DETAIL = re.compile(r"ttt2:\s+(wait|kernel calls|guest stack|busy|calls by site|holds|object): (.*)$")
RE_HOLD = re.compile(r"critical section (0x[0-9A-Fa-f]+) at depth (\d+) since t\+([\d.]+) s \(the section's own count: "
                     r"(-?\d+)\), unchanged for ([\d.]+) s; (\d+) enters and (\d+) leaves since then; open enters from (.*)$")
RE_HOLD_STACK = re.compile(r"critical section (0x[0-9A-Fa-f]+) entered to depth (\d+) at t\+([\d.]+) s: (.*)$")
# Diagnostics of the game's own calls (recomp/src/diag_overrides.cpp, wait_overrides.cpp).
# "... from <call site> on <thread>": a thread label may hold parentheses ("nuscMain (id 7)"), a call site never " on ".
RE_THREAD_NAME = re.compile(r"ttt2: thread name '(.*)' for thread id (-1: the caller|\d+): (.*); set from (.*) on (.*)$")
RE_CPP_EXCEPTION = re.compile(r"ttt2: C\+\+ exception (\d+) thrown \((.*?)\) from (.*) on (.*?): the runtime cannot unwind")
RE_OTHER_EXCEPTION = re.compile(r"ttt2: exception (0x[0-9A-Fa-f]+) raised \(flags (0x[0-9A-Fa-f]+), address (0x[0-9A-Fa-f]+)\) "
                                r"from (.*) on (.*)$")
RE_DEBUG_BREAK = re.compile(r"ttt2: debug break (\d+) from (.*) on (.*)$")
RE_BUG_CHECK = re.compile(r"ttt2: bug check (0x[0-9A-Fa-f]+)(.*?) from (.*) on (.*)$")
RE_MEMORY_FAILURE = re.compile(r"ttt2: memory: (.*) -> (0x[0-9A-Fa-f]{8}(?: \([A-Z_]+\))?) from (.*) on (.*)$")
RE_APC = re.compile(r"ttt2: APC (\d+) queued to handle (0x[0-9A-Fa-f]+): routine (.*) \(context (0x[0-9A-Fa-f]+)\) -> (.*?) "
                    r"from (.*) on (.*)$")
RE_SIGNAL_WAIT_MODE = re.compile(r"ttt2: NtSignalAndWaitForSingleObjectEx from (.*): wait mode (\d+), alertable (\d+) \((.*)\)$")
RE_DUMP_BUSIEST = re.compile(r"ttt2:\s+traced kernel calls in the last tick: (.*)$")
RE_DUMP_FILES = re.compile(r"ttt2:\s+recent file activity")
RE_DUMP_FILE_EVENT = re.compile(r"ttt2:\s+t\+([\d.]+) s (\w+) '(.*)' -> (.*)$")
RE_TRACED = re.compile(r"ttt2: (\d+) kernel exports traced by the host")
RE_ACTIVITY = re.compile(r"ttt2: activity: (.*)$")
RE_FILE_OPENED = re.compile(r"ttt2: file opened: '(.*)' \((\w+), (.*)\)$")
RE_FILE_FAILED = re.compile(r"ttt2: file open failed: '(.*)' -> (0x[0-9A-Fa-f]+)(?: \((\w+)\))? \((\w+), (.*), from (.*)\)$")
RE_DEVICE_OPEN = re.compile(r"ttt2: the game opened '(.*)', a device itself")
RE_SECTOR = re.compile(r"ttt2: sector query: FileSectorInformation for '(.*)' answered as unsupported")
RE_ATTR_MISSING = re.compile(r"ttt2: file attributes missing: '(.*)' -> (0x[0-9A-Fa-f]+)")
RE_READ_FAILED = re.compile(r"ttt2: file read failed: '(.*)' at (\S+) length (\d+) -> (0x[0-9A-Fa-f]+)")
RE_DEVICE_CONTROL = re.compile(r"ttt2: device control (0x[0-9A-Fa-f]+) on '(.*)' -> (0x[0-9A-Fa-f]+)")
RE_SITE = r"(0x[0-9A-Fa-f]{8}(?: \([^)]*\))?)"
RE_FILE_INFO = re.compile(r"ttt2: file information: (query|set|volume query) class (\d+) \((.*?)\) for '(.*)' -> "
                          r"(0x[0-9A-Fa-f]{8})(?: \(([A-Z_]+)\))? from " + RE_SITE + r" on (.*)$")
RE_FILE_READ = re.compile(r"ttt2: file read (\d+): '(.*)' at (.+?) length (\d+) -> (0x[0-9A-Fa-f]{8})(?: \([A-Z_]+\))?, "
                          r"(\d+) bytes(.*?) from " + RE_SITE + r" on ([^;]*)")
RE_FILE_WRITE = re.compile(r"ttt2: file write (failure )?(\d+): '(.*)' at (.+?) length (\d+) -> (0x[0-9A-Fa-f]{8})"
                           r"(?: \([A-Z_]+\))?, (\d+) bytes from " + RE_SITE + r" on (.*)$")
RE_DIR_QUERY = re.compile(r"ttt2: directory query (\d+): '(.*)' for '(.*)' -> (0x[0-9A-Fa-f]{8})(?: \(([A-Z_]+)\))? from "
                          + RE_SITE + r" on ([^;]*)")
# Information classes the reference ports answer (recomp/src/io_overrides.cpp): TTT2Recomp answers these and any other
# class with success; TTT2 Web answers 5, 14 and 34 and refuses 27, which it takes for the sector query.
TTT2RECOMP_FILE_CLASSES = (4, 5, 6, 7, 14, 27, 34)
REFUSED_STATUSES = {"0xC0000002", "0xC0000003", "0xC000000D"}  # not implemented, invalid class, invalid parameter
RE_MILESTONE = re.compile(r"ttt2: (gpu|audio|input|xam): (.*)$")
RE_THREAD_CREATED = re.compile(r"ttt2: thread created: id (\d+), start (0x[0-9A-Fa-f]+)(?: \((sub_[0-9A-Fa-f]+)\))?(.*)$")
RE_OWNER_ID = re.compile(r"owned by (?:.*?\(id (\d+)\)|thread id (\d+))")
RE_CLOSE_ENDED = re.compile(r"ttt2: main guest thread ended by the window close \(id (\d+)\)")
RE_KERNEL_CLOCK = re.compile(r"ttt2: kernel clock: (.*)$")
RE_MISSING_NAME = re.compile(r"ttt2: missing name: '(.*)' \((\w+)\) answered as (0x[0-9A-Fa-f]+)")
# Frame monitor lines (recomp/src/frame_monitor.cpp), matched on the text after "ttt2: gpu: ".
RE_FRAME_STATS = re.compile(r"^frame (\d+): (.*) \((\d+) words of commands\)$")
RE_FIRST_DRAWS = re.compile(r"^first frame with draws: frame (\d+): (.*) \((?:[^()]|\([^()]*\))*, from .*\)$")
RE_SKIPPED_PACKETS = re.compile(r"^frame (\d+) holds packets the GPU emulation does not execute.*?: (.*)$")
RE_FRONT_BUFFER = re.compile(r"^front buffer (\d+)x(\d+) (\S+) at (0x[0-9A-Fa-f]+) \(frame (\d+)\)$")
RE_PIXEL = re.compile(r"^pixel check frame (\d+): (.*)$")
# A thread dump's "kernel calls" text from hosts without the busy line: "<N> in the last tick ...".
RE_CALLS_LAST_TICK = re.compile(r"^(\d+) in the last tick")
RE_CS_WAIT = re.compile(r"critical section (0x[0-9A-Fa-f]+) \(owned by (.*?), recursion (-?\d+)\)")
# Traced kernel calls in one watchdog tick (5 s by default) from which a thread counts as spinning, for logs of
# hosts that do not flag busy threads themselves (the host's own threshold is 50000 a second).
BUSY_CALLS_PER_TICK = 250000
# Device paths the game opens on every boot that the reference ports (TTT2 Web, TTT2Recomp) do not map either:
# their failure is expected and not what stops a boot.
KNOWN_DEVICE_PROBES = {"\\device\\image"}
# The activity line's fields, read one by one (the GPU part depends on the backend).
ACTIVITY_FIELDS = {
    "files_opened": r"files (\d+) opened", "open_failures": r"opened, (\d+) failed", "device_failures": r"\((\d+) device paths\)",
    "mb_read": r"read ([\d.]+) MB", "reads": r"in (\d+) reads", "read_failures": r"reads \((\d+) failed\)",
    "sector_queries": r"sector queries (\d+)", "threads_created": r"threads created (\d+)",
    "engines": r"engines (initialized|not initialized)", "ring_buffer": r"ring buffer (0x[0-9A-Fa-f]+|not set up)",
    "interrupt_callback": r"interrupt callback (0x[0-9A-Fa-f]+|none)", "vblanks": r"vblanks (\d+)",
    "ring_write_pointer": r"ring write pointer (0x[0-9A-Fa-f]+)", "frames_presented": r"frames presented (\d+)",
    "audio_clients": r"audio clients (\d+)", "audio_frames": r"audio frames (\d+)", "input_polls": r"input polls (\d+)",
    "traced_calls": r"traced kernel calls (\d+)",
    "missing_names": r"missing names (\d+)",
    "kernel_clock_s": r"kernel clock ([\d.]+) s", "tick_count_s": r"tick count ([\d.]+) s",
    "kernel_clock_state": r"kernel clock (off|missing|not started)",
    "frames_with_draws": r"frames presented \d+ \((?:\d+ followed in the GPU command stream by [^;]+; )?(\d+) with draws",
    "frames_with_resolves": r"(\d+) with resolves",
    # Which frames the host could read: newer hosts follow the GPU's command stream (by the ring's write pointer with
    # the GPU emulation, else between swaps in the ring) and count the rest as not followed; older ones read only the
    # ring between swaps and counted the swaps outside it.
    "frames_followed": r"frames presented \d+ \((\d+) followed in the GPU command stream",
    "follow_method": r"followed in the GPU command stream by (the ring write pointer|the ring between swaps|nothing)",
    "frames_not_followed": r"followed in the GPU command stream by [^,;]+, (\d+) not followed",
    "swaps_outside_ring": r"(\d+) swaps? outside the ring",
    "draws_total": r"; draws (\d+), resolves \d+, clears \d+", "resolves_total": r"; draws \d+, resolves (\d+), clears \d+",
    "clears_total": r"; draws \d+, resolves \d+, clears (\d+)", "max_draws": r"at most (\d+) draws in a frame",
    "skipped_packets": r"clears \d+(?:, at most \d+ draws in a frame)?, (\d+) packets? the GPU emulation skips",
    "last_frame": r"last frame: (.*?)(?:; front buffer \d+x\d+)?\); audio clients",
    "front_buffer": r"; front buffer (\d+x\d+)\); audio clients", "frame_stats_off": r"(frame stats off)",
    "exceptions_raised": r"exceptions raised (\d+)", "cpp_exceptions": r"exceptions raised \d+ \((\d+) C\+\+\)",
    "apcs_queued": r"APCs queued (\d+)", "memory_failures": r"memory call failures (\d+)",
    "debug_breaks": r"debug breaks (\d+)",
}
FLOAT_FIELDS = {"mb_read", "kernel_clock_s", "tick_count_s"}
RE_REPORT = re.compile(r"ttt2: kernel stub report at (.*): (\d+) calls to (\d+) distinct stubs")
RE_REPORT_LINE = re.compile(r"ttt2:\s+(\S+)\s+(\d+) calls \(first seen as stub call #(\d+)\)")
RE_CRASH = re.compile(r"ttt2: HOST CRASH: (.*) at host pc (0x[0-9a-fA-F]+)")
RE_CRASH_EXIT = re.compile(r"ttt2: exiting with code (\d+) after the host crash")
RE_CRASH_FAULT = re.compile(r"ttt2:\s+fault address (0x[0-9a-fA-F]+)")
RE_STUB_WARN = re.compile(r"^(?:__imp__)?(\w+) STUB$")
RE_AV = re.compile(r"Unhandled guest access violation: (read|write) of guest (0x[0-9A-Fa-f]+)")
RE_ARENA = re.compile(r"Guest memory arena mapped: virtual base (0x[0-9a-fA-F]+), physical base (0x[0-9a-fA-F]+)")
RE_VAR_IMPORT = re.compile(r"Variable import (\w+):(\S+) \((\w+)\) not implemented")
RE_GPU_MISSING = re.compile(r"GPU plugin '([^']+)' not found")
RE_NOT_TTT2 = re.compile(r"ttt2: this executable is not Tekken Tag Tournament 2")
# The runtime's trap for an indirect call (bctrl / bctr through a pointer) to an address no recompiled
# function starts at: REX_FATAL logs it and aborts the process.
RE_INVALID_FUNC = re.compile(r"Call to invalid or unregistered function at guest address (0x[0-9A-Fa-f]+)")

TTT2_TITLE_ID = "4E4D0859"


def session_files(path):
    """The files of one log session, oldest first. A file is used as is; a directory yields the newest
    session's base file and its rotated parts (`<base>.N.log`, higher N is older)."""
    if os.path.isfile(path):
        return [path]
    if not os.path.isdir(path):
        raise SystemExit(f"not a file or directory: {path}")
    bases = {}
    for f in glob.glob(os.path.join(path, "*.log")):
        name = os.path.basename(f)
        m = re.match(r"^(.*?)(?:\.(\d+))?\.log$", name)
        if not m:
            continue
        base, index = m.group(1), int(m.group(2)) if m.group(2) else 0
        bases.setdefault(base, []).append((index, f))
    if not bases:
        raise SystemExit(f"no .log files in {path}")
    newest = max(bases, key=lambda b: max(os.path.getmtime(f) for _, f in bases[b]))
    parts = sorted(bases[newest], key=lambda t: t[0], reverse=True)  # highest index = oldest
    return [f for _, f in parts]


def parse_time(ts):
    return datetime.strptime(ts, "%Y-%m-%d %H:%M:%S.%f")


def norm_hex(text):
    """0xABC form: the digits upper-case, the prefix lower-case (as the host prints guest addresses)."""
    return "0x" + text[2:].upper() if text.lower().startswith("0x") else text


def norm_status(text):
    """0xc0000003 -> 0xC0000003."""
    return "0x" + text[2:].upper()


def normalise(message):
    """Collapses numbers and hex so repeated messages with changing addresses group together."""
    return re.sub(r"0x[0-9A-Fa-f]+|\b\d+\b", "N", message)


def guest_address(host_address, arena):
    """Translates a host address into the guest address it maps, using the logged arena bases."""
    if not arena:
        return None
    vbase, pbase = arena
    if vbase <= host_address < vbase + 0x100000000:
        return ("virtual", host_address - vbase)
    if pbase <= host_address < pbase + 0x100000000:
        return ("physical", host_address - pbase)
    return None


def region_of(guest_addr):
    if guest_addr < 0x10000:
        return "null page (a null or near-null pointer)"
    if 0x40000000 <= guest_addr < 0x70000000:
        return "user heap (0x40000000-0x6FFFFFFF)"
    if 0x70000000 <= guest_addr < 0x7F000000:
        return "guest thread stacks (0x70000000-0x7EFFFFFF)"
    if 0x80000000 <= guest_addr < 0x90000000:
        return "the executable image (0x80000000-0x8FFFFFFF)"
    if 0x90000000 <= guest_addr < 0xA0000000:
        return "kernel/module area (0x90000000-0x9FFFFFFF)"
    if guest_addr >= 0xA0000000:
        return "physical memory (0xA0000000 and above)"
    return "low virtual memory (below 0x40000000)"


def parse_activity(text):
    """The watchdog's activity line as a dict of its fields (numbers as int/float, the rest as text)."""
    out = {}
    for key, pattern in ACTIVITY_FIELDS.items():
        m = re.search(pattern, text)
        if not m:
            continue
        value = m.group(1)
        if key in FLOAT_FIELDS:
            out[key] = float(value)
        elif key == "frame_stats_off":
            out[key] = True
        elif value.isdigit():
            out[key] = int(value)
        else:
            out[key] = value
    out["gpu_emulation"] = "no GPU emulation" not in text
    return out


def frame_coverage(a):
    """(frames whose commands were read, frames presented whose commands were not, how) from an activity dict.
    Older hosts read the ring between swaps and only counted the swaps outside it."""
    presented = a.get("frames_presented", 0)
    if "frames_followed" in a:
        return a["frames_followed"], a.get("frames_not_followed", 0), a.get("follow_method", "?")
    outside = a.get("swaps_outside_ring", 0)
    return max(0, presented - outside), outside, "the ring between swaps"


def owner_ids(wait_text):
    """Thread ids named as owners in a wait line ("owned by nuscMain (id 7)", "owned by thread id 9")."""
    return [int(a or b) for a, b in RE_OWNER_ID.findall(wait_text or "")]


def find_deadlocks(threads):
    """Cycles in the wait-for graph of one dump: thread -> the owner of what it waits on."""
    edges = {}
    for th in threads:
        owners = owner_ids(th.get("wait"))
        if owners:
            edges[th["id"]] = owners[0]
    cycles, seen = [], set()
    for start in edges:
        path, node = [], start
        while node in edges and node not in path:
            path.append(node)
            node = edges[node]
        if node in path:
            cycle = path[path.index(node):]
            key = frozenset(cycle)
            if key not in seen:
                seen.add(key)
                cycles.append(cycle)
    return cycles


def triage(lines, ttt2web=None, closed_by=None):
    """Returns the structured triage of the given log lines (dicts and lists only). `closed_by` names who closed the
    window when it was not a person (run-host.ps1's timeout: "the boot step's 180 s timeout")."""
    ttt2web = ttt2web or {}
    facts = {"title_id": None, "version": None, "base_version": None, "media_id": None, "not_ttt2": False,
             "graphics_backend": None, "roots": {}, "imports": {}, "arena": None}
    events = []  # (timestamp, text)
    overrides = []
    ticks = []
    stalls = []
    report = None
    crash = None
    stub_warnings = collections.Counter()
    access_violations = collections.Counter()
    first_av = None
    var_imports = []
    gpu_missing = []
    invalid_calls = []
    errors = collections.Counter()
    first_error_text = {}
    levels = collections.Counter()
    first_ts = last_ts = None
    main_created = main_exited = ended = closed = None
    too_few_cores = False
    current_stall = None
    current_dump = None  # the dump whose per-thread lines are being read (a stall dump or the close dump)
    close_dump = None
    activity = None
    traced_exports = None
    files = {"opened": [], "failed": [], "device_opens": [], "sector_first": None, "attributes_missing": [],
             "read_failures": [], "device_controls": [], "info": [], "reads": [], "writes": [], "dir_queries": []}
    milestones = []
    threads_created = []
    kernel_clock = None
    missing_names = []
    frames = {"first": [], "first_with_draws": None, "skipped_packets": None, "front_buffers": [], "pixel_checks": []}
    ended_by_close = None
    unparsed = 0
    # The game's own calls: thread names, exceptions, debug breaks, APCs, refused memory calls, signal-and-wait flags.
    calls = {"thread_names": [], "cpp_exceptions": [], "other_exceptions": [], "debug_breaks": [], "bug_checks": [],
             "memory_failures": [], "apcs": [], "signal_and_wait": []}

    for raw in lines:
        line = raw.rstrip("\r\n")
        if not line:
            continue
        m = RE_LINE.match(line)
        if not m:
            unparsed += 1
            continue
        ts, level, _category, _thread, msg = m.groups()
        levels[level] += 1
        first_ts = first_ts or ts
        last_ts = ts
        if level in ("error", "critical"):
            key = normalise(msg)
            errors[key] += 1
            first_error_text.setdefault(key, msg)

        if (mm := RE_ARENA.search(msg)):
            facts["arena"] = (int(mm.group(1), 16), int(mm.group(2), 16))
        elif (mm := RE_INVALID_FUNC.search(msg)):
            addr = norm_hex(mm.group(1))
            if addr not in [c["address"] for c in invalid_calls]:
                invalid_calls.append({"time": ts, "address": addr})
            events.append((ts, f"FATAL: indirect call to {addr}, where no recompiled function starts"))
        elif (mm := RE_TITLE.search(msg)):
            facts.update(title_id=mm.group(1).upper(), version=mm.group(2), base_version=mm.group(3),
                         media_id=mm.group(4).upper())
            events.append((ts, f"executable identified: title {facts['title_id']} version {facts['version']}"))
        elif RE_NOT_TTT2.search(msg):
            facts["not_ttt2"] = True
        elif (mm := RE_BACKEND.search(msg)):
            facts["graphics_backend"] = mm.group(1).strip()
        elif (mm := RE_ROOT.search(msg)):
            facts["roots"][mm.group(1)] = mm.group(2).strip()
        elif (mm := RE_IMPORTS.search(msg)):
            facts["imports"][mm.group(2)] = int(mm.group(1))
        elif (mm := RE_MAIN_CREATED.search(msg)):
            main_created = ts
            events.append((ts, f"main guest thread created (id {mm.group(1)})"))
        elif (mm := RE_MAIN_EXITED.search(msg)):
            main_exited = ts
            events.append((ts, f"main guest thread exited (id {mm.group(1)})"))
        elif (mm := RE_CLOSE_ENDED.search(msg)):
            ended_by_close = ts
            events.append((ts, f"main guest thread ended by the window close (id {mm.group(1)})"))
        elif (mm := RE_KERNEL_CLOCK.search(msg)):
            kernel_clock = mm.group(1)
        elif (mm := RE_MISSING_NAME.search(msg)):
            missing_names.append({"time": ts, "path": mm.group(1), "api": mm.group(2), "status": mm.group(3)})
        elif (mm := RE_ENDED.search(msg)):
            ended = int(mm.group(1))
            events.append((ts, f"host exited with code {ended} after the program ended"))
        elif (mm := RE_OVERRIDE.search(msg)):
            overrides.append({"name": mm.group(1), "result": mm.group(2), "time": ts})
        elif (mm := RE_WATCHDOG.search(msg)):
            ticks.append({"time": ts, "threads": int(mm.group(1)), "running": int(mm.group(2)),
                          "main_running": mm.group(3) == "running", "names": mm.group(4), "stubs": mm.group(5)})
            current_stall = current_dump = None
        elif (mm := RE_ACTIVITY.search(msg)):
            activity = {"time": ts, **parse_activity(mm.group(1))}
            current_dump = None
        elif (mm := RE_STALL.search(msg)):
            current_stall = {"time": ts, "unchanged_ms": int(mm.group(1)), "still": False, "threads": [],
                             "busiest": None, "files": []}
            current_dump = current_stall
            stalls.append(current_stall)
            events.append((ts, f"possible stall: nothing changed for {mm.group(1)} ms"))
        elif (mm := RE_STILL_STALLED.search(msg)):
            current_stall = {"time": ts, "unchanged_ms": int(mm.group(1)), "still": True, "threads": [],
                             "busiest": None, "files": []}
            current_dump = current_stall
            stalls.append(current_stall)
            events.append((ts, f"still stalled after {mm.group(1)} ms"))
        elif (mm := RE_DUMP_HEADER.search(msg)):
            close_dump = {"time": ts, "when": mm.group(1), "threads": [], "busiest": None, "files": []}
            current_dump = close_dump
        elif current_dump is not None and (mm := RE_STALL_THREAD.search(msg)):
            current_dump["threads"].append({"name": mm.group(1), "id": int(mm.group(2)), "state": mm.group(3),
                                            "priority": int(mm.group(4)), "suspend": int(mm.group(5)),
                                            "wait_result": mm.group(6), "wait": None, "calls": None, "stack": None,
                                            "busy": None, "sites": None, "holds": []})
        elif current_dump is not None and current_dump["threads"] and (mm := RE_THREAD_DETAIL.search(msg)):
            if mm.group(1) in ("holds", "object"):
                key = "holds" if mm.group(1) == "holds" else "objects"
                current_dump["threads"][-1].setdefault(key, []).append(mm.group(2))
            else:
                key = {"wait": "wait", "kernel calls": "calls", "guest stack": "stack", "busy": "busy",
                       "calls by site": "sites"}[mm.group(1)]
                current_dump["threads"][-1][key] = mm.group(2)
        elif current_dump is not None and (mm := RE_DUMP_BUSIEST.search(msg)):
            current_dump["busiest"] = mm.group(1)
        elif current_dump is not None and RE_DUMP_FILES.search(msg):
            pass
        elif current_dump is not None and (mm := RE_DUMP_FILE_EVENT.search(msg)):
            current_dump["files"].append(f"t+{mm.group(1)} s {mm.group(2)} '{mm.group(3)}' -> {mm.group(4)}")
        elif (mm := RE_TRACED.search(msg)):
            traced_exports = int(mm.group(1))
        elif (mm := RE_FILE_OPENED.search(msg)):
            files["opened"].append({"time": ts, "path": mm.group(1), "api": mm.group(2), "thread": mm.group(3)})
        elif (mm := RE_FILE_FAILED.search(msg)):
            files["failed"].append({"time": ts, "path": mm.group(1), "status": mm.group(2).upper().replace("0X", "0x"),
                                    "status_name": mm.group(3), "api": mm.group(4), "thread": mm.group(5),
                                    "caller": mm.group(6)})
        elif (mm := RE_DEVICE_OPEN.search(msg)):
            files["device_opens"].append({"time": ts, "path": mm.group(1)})
            events.append((ts, f"the game opened the bare device {mm.group(1)} (the open failed)"))
        elif (mm := RE_SECTOR.search(msg)):
            files["sector_first"] = {"time": ts, "path": mm.group(1)}
            events.append((ts, "first disc sector query, answered as unsupported by the host"))
        elif (mm := RE_ATTR_MISSING.search(msg)):
            files["attributes_missing"].append({"path": mm.group(1), "status": mm.group(2)})
        elif (mm := RE_READ_FAILED.search(msg)):
            files["read_failures"].append({"path": mm.group(1), "offset": mm.group(2), "length": int(mm.group(3)),
                                           "status": mm.group(4)})
        elif (mm := RE_DEVICE_CONTROL.search(msg)):
            files["device_controls"].append({"code": mm.group(1), "path": mm.group(2), "status": mm.group(3)})
        elif (mm := RE_FILE_INFO.search(msg)):
            files["info"].append({"time": ts, "what": mm.group(1), "class": int(mm.group(2)), "name": mm.group(3),
                                  "path": mm.group(4), "status": norm_status(mm.group(5)), "status_name": mm.group(6),
                                  "caller": mm.group(7), "thread": mm.group(8)})
        elif (mm := RE_FILE_READ.search(msg)):
            files["reads"].append({"time": ts, "n": int(mm.group(1)), "path": mm.group(2), "offset": mm.group(3),
                                   "length": int(mm.group(4)), "status": norm_status(mm.group(5)),
                                   "bytes": int(mm.group(6)), "completion": mm.group(7).lstrip("; "),
                                   "caller": mm.group(8), "thread": mm.group(9).strip()})
        elif (mm := RE_FILE_WRITE.search(msg)):
            files["writes"].append({"time": ts, "failed": bool(mm.group(1)), "path": mm.group(3), "offset": mm.group(4),
                                    "length": int(mm.group(5)), "status": norm_status(mm.group(6)),
                                    "bytes": int(mm.group(7)), "caller": mm.group(8), "thread": mm.group(9)})
        elif (mm := RE_DIR_QUERY.search(msg)):
            files["dir_queries"].append({"time": ts, "path": mm.group(2), "pattern": mm.group(3),
                                         "status": norm_status(mm.group(4)), "status_name": mm.group(5),
                                         "caller": mm.group(6), "thread": mm.group(7).strip()})
        elif (mm := RE_THREAD_CREATED.search(msg)):
            threads_created.append({"time": ts, "id": int(mm.group(1)), "start": mm.group(2),
                                    "function": mm.group(3), "details": mm.group(4).strip()})
        elif (mm := RE_MILESTONE.search(msg)):
            milestones.append({"time": ts, "area": mm.group(1), "text": mm.group(2)})
            text = mm.group(2)
            if mm.group(1) == "gpu":
                if text.startswith("first frame presented"):
                    events.append((ts, "first frame presented (VdSwap)"))
                elif (mf := RE_FRAME_STATS.match(text)):
                    frames["first"].append({"frame": int(mf.group(1)), "stats": mf.group(2), "words": int(mf.group(3))})
                elif (mf := RE_FIRST_DRAWS.match(text)):
                    frames["first_with_draws"] = {"time": ts, "frame": int(mf.group(1)), "stats": mf.group(2)}
                    events.append((ts, f"first frame with draws (frame {mf.group(1)})"))
                elif (mf := RE_SKIPPED_PACKETS.match(text)):
                    frames["skipped_packets"] = {"frame": int(mf.group(1)), "stats": mf.group(2)}
                elif (mf := RE_FRONT_BUFFER.match(text)):
                    frames["front_buffers"].append({"width": int(mf.group(1)), "height": int(mf.group(2)),
                                                    "format": mf.group(3), "address": mf.group(4), "frame": int(mf.group(5))})
                elif (mf := RE_PIXEL.match(text)):
                    frames["pixel_checks"].append({"frame": int(mf.group(1)), "text": mf.group(2)})
        elif (mm := RE_REPORT.search(msg)):
            report = {"when": mm.group(1), "calls": int(mm.group(2)), "distinct": int(mm.group(3)), "stubs": []}
        elif report is not None and (mm := RE_REPORT_LINE.search(msg)):
            report["stubs"].append({"name": mm.group(1), "calls": int(mm.group(2)), "first_seen": int(mm.group(3))})
        elif (mm := RE_CRASH.search(msg)):
            crash = {"time": ts, "kind": mm.group(1), "host_pc": mm.group(2), "details": [], "fault_address": None,
                     "exit_code": None}
            events.append((ts, f"HOST CRASH: {mm.group(1)}"))
        elif crash is not None and (mm := RE_CRASH_EXIT.search(msg)):
            crash["exit_code"] = int(mm.group(1))
        elif crash is not None and msg.startswith("ttt2:   "):
            crash["details"].append(msg[len("ttt2:   "):].strip())
            if (mf := RE_CRASH_FAULT.search(msg)):
                crash["fault_address"] = mf.group(1)
        elif (mm := RE_STUB_WARN.match(msg)):
            stub_warnings[mm.group(1)] += 1
        elif (mm := RE_AV.search(msg)):
            access_violations[(mm.group(1), norm_hex(mm.group(2)))] += 1
            first_av = first_av or (ts, mm.group(1), norm_hex(mm.group(2)))
        elif (mm := RE_VAR_IMPORT.search(msg)):
            var_imports.append({"library": mm.group(1), "ordinal": mm.group(2), "name": mm.group(3)})
        elif (mm := RE_GPU_MISSING.search(msg)):
            gpu_missing.append(mm.group(1))
        elif (mm := RE_THREAD_NAME.search(msg)):
            calls["thread_names"].append({"time": ts, "name": mm.group(1), "id": mm.group(2), "applied": mm.group(3),
                                          "from": mm.group(4), "thread": mm.group(5)})
        elif (mm := RE_CPP_EXCEPTION.search(msg)):
            calls["cpp_exceptions"].append({"time": ts, "n": int(mm.group(1)), "type": mm.group(2), "from": mm.group(3),
                                            "thread": mm.group(4)})
        elif (mm := RE_OTHER_EXCEPTION.search(msg)):
            calls["other_exceptions"].append({"time": ts, "code": mm.group(1), "from": mm.group(4), "thread": mm.group(5)})
        elif (mm := RE_DEBUG_BREAK.search(msg)):
            calls["debug_breaks"].append({"time": ts, "from": mm.group(2), "thread": mm.group(3)})
        elif (mm := RE_BUG_CHECK.search(msg)):
            calls["bug_checks"].append({"time": ts, "code": mm.group(1), "from": mm.group(3), "thread": mm.group(4)})
        elif (mm := RE_MEMORY_FAILURE.search(msg)):
            calls["memory_failures"].append({"time": ts, "call": mm.group(1), "status": mm.group(2), "from": mm.group(3),
                                             "thread": mm.group(4)})
        elif (mm := RE_APC.search(msg)):
            calls["apcs"].append({"time": ts, "handle": mm.group(2), "routine": mm.group(3), "status": mm.group(5),
                                  "from": mm.group(6), "thread": mm.group(7)})
        elif (mm := RE_SIGNAL_WAIT_MODE.search(msg)):
            calls["signal_and_wait"].append({"from": mm.group(1), "wait_mode": int(mm.group(2)),
                                             "alertable": int(mm.group(3)), "note": mm.group(4)})
        elif "Window closing, shutting down" in msg:
            closed = ts
            events.append((ts, f"window closed by {closed_by}" if closed_by else "window closed by the user"))
        elif "Too few processor cores" in msg:
            too_few_cores = True

    # Outcome.
    if crash:
        outcome = f"crash: {crash['kind']}"
    elif invalid_calls:
        outcome = (f"fatal: the game called {invalid_calls[0]['address']} through a pointer, where no recompiled "
                   "function starts (the runtime aborts on such a call)")
    elif ended is not None:
        outcome = "clean exit: the program returned from its entry point and the host ended"
    elif stalls:
        # The owner's most likely first boot: a black or frozen window, the stall dump, then the X button.
        outcome = "stall: the thread census, stub calls and boot progress stopped changing while the main thread ran"
        if closed:
            outcome += "; the window was closed afterwards"
    elif closed:
        outcome = (f"closed by {closed_by} while the game was running" if closed_by
                   else "closed by the user while the game was running")
    elif main_created:
        outcome = "incomplete: the log ends while the game runs (killed, still running, or the log was cut)"
    elif facts["title_id"]:
        outcome = "the host identified the image but the module was never launched (setup failure)"
    else:
        outcome = "the host did not get as far as loading the image"

    duration_s = None
    if first_ts and last_ts:
        duration_s = round((parse_time(last_ts) - parse_time(first_ts)).total_seconds(), 3)

    # Kernel stubs: the report if the host wrote one, else the raw STUB warnings.
    if report and report["stubs"]:
        stubs = sorted(report["stubs"], key=lambda s: -s["calls"])
        stub_source = f"kernel stub report at {report['when']}"
    else:
        stubs = [{"name": n, "calls": c, "first_seen": None} for n, c in stub_warnings.most_common()]
        stub_source = "STUB warnings in the log" if stubs else "none reached"
    for s in stubs:
        ref = ttt2web.get(s["name"])
        s["ttt2web"] = ref[0] if ref else None
        s["ttt2web_kind"] = ref[1] if ref else None

    recommendations = []
    if facts["title_id"] and facts["title_id"] != TTT2_TITLE_ID:
        recommendations.append(f"The image is title {facts['title_id']}, not Tekken Tag Tournament 2 ({TTT2_TITLE_ID}): "
                               "a test image, or the wrong file was given as default.xex.")
    if gpu_missing:
        recommendations.append(f"GPU plugin '{gpu_missing[0]}' was not found next to the executable: the build stages "
                               "rexgpu-xenos there (scripts/build-host.ps1); copy it or rebuild.")
    if facts["graphics_backend"] == "none":
        recommendations.append("GPU emulation was off (--gpu_plugin=none or no plugin staged): the game cannot draw this "
                               "way; only use it to gather logs. The default loads the staged Xenos plugin.")
    if crash:
        guest = None
        if crash["fault_address"]:
            guest = guest_address(int(crash["fault_address"], 16), facts["arena"])
        where = ""
        if guest:
            where = f" The fault address maps to guest {guest[0]} address 0x{guest[1]:08X}: {region_of(guest[1])}."
        recommendations.append(f"Crash ({crash['kind']}) at host pc {crash['host_pc']}.{where} Resolve the host pc with the "
                               "build's symbols (llvm-symbolizer / addr2line on ttt2, or the .pdb on Windows) to find the "
                               "generated function, then the guest address it came from (recomp/generated).")
        if "illegal instruction" in crash["kind"]:
            recommendations.append("An illegal instruction in recompiled code is a REX_FATAL / trap: an unresolved call, an "
                                   "unimplemented instruction, a jump-table miss or a jump into data. Check the codegen "
                                   "report (REPORT.md) for unresolved targets and unimplemented sites first.")
    for c in invalid_calls:
        recommendations.append(
            f"Indirect call to {c['address']}, where ReXGlue registered no function: it registers bl targets, .pdata, "
            "RTTI vtables, lis/addi code pointers and its gap fill, so a function reached only through another pointer "
            "(a vtable without RTTI, a callback table) is missing. recomp/tools/resolve_functions.py boot adds it as an "
            "entry when the image has code there and TTT2 Web's table lists a function starting there (the owner "
            "pipeline does this after -Boot and rebuilds); an address inside a function is a jump into its middle, "
            "usually a jump table the switch-table finder did not recognise.")
    if access_violations:
        total = sum(access_violations.values())
        (op, addr), n = access_violations.most_common(1)[0]
        g = int(addr, 16)
        note = f"{total} unhandled guest access violation(s), most often a {op} of 0x{g:08X} ({region_of(g)})."
        if total > 1000:
            note += (" Thousands of repeats mean the fault re-executed in a loop: a host older than the crash-exit fix, "
                     "or TTT2_CRASH_KEEP_RUNNING was set.")
        recommendations.append(note)
    for s in stubs:
        if s["ttt2web"]:
            recommendations.append(f"Stub {s['name']} reached {s['calls']}x: implement it as a host override "
                                   f"(recomp/src/kernel_overrides.cpp); TTT2 Web (CC0) {s['ttt2web_kind']} it at {s['ttt2web']}.")
        else:
            recommendations.append(f"Stub {s['name']} reached {s['calls']}x: implement it as a host override "
                                   "(recomp/src/kernel_overrides.cpp); TTT2 Web does not define it, so the game ran "
                                   "without it there (check what the game does with the result).")
    for v in var_imports:
        recommendations.append(f"Variable import {v['name']} ({v['library']} ordinal {v['ordinal']}) is not implemented by "
                               "the runtime, which wrote a poison value into its slot: any use of it is a crash candidate.")
    for st in stalls:
        if st.get("still"):
            continue
        waiting = [t for t in st["threads"] if t["state"] != "running"]
        recommendations.append(f"Stall after {st['unchanged_ms']} ms without change: {len(st['threads'])} guest threads, "
                               f"{len(waiting)} not running. Look at what the running threads wait on (events, vblank, a "
                               "stubbed call that never signals) in the per-thread dump.")
    # The boot monitor's view (hosts with recomp/src/boot_monitor.cpp).
    last_dump = close_dump or (stalls[-1] if stalls else None)
    probes = sorted({d["path"] for d in files["device_opens"] if d["path"].lower() in KNOWN_DEVICE_PROBES})
    for path in probes:
        statuses = sorted({f["status"] for f in files["failed"] if f["path"] == path})
        text = (f"The game probed the device {path} ({', '.join(statuses) or 'failed'}): TTT2 does this on every boot, and "
                "TTT2 Web and TTT2Recomp, which run the game to its menus, have no such device either and answer it as a "
                "missing name; this is not what stops the boot.")
        if "0xC000000F" in statuses:
            text += (" This host answered with the runtime's STATUS_NO_SUCH_FILE; newer hosts give the console's and the "
                     "reference ports' answer (STATUS_OBJECT_NAME_NOT_FOUND, the caller's handle left alone).")
        recommendations.append(text)
    if files["device_opens"] and any(d["path"].lower() not in KNOWN_DEVICE_PROBES for d in files["device_opens"]):
        paths = sorted({d["path"] for d in files["device_opens"] if d["path"].lower() not in KNOWN_DEVICE_PROBES})
        if files["sector_first"]:
            recommendations.append(
                f"The game opened the bare device {', '.join(paths)} although the host answered its disc sector query as "
                "unsupported: it wants the device itself, not raw sectors behind a successful sector query. That open is "
                "the runtime's \"ResolvePath(\\Device) failed\" error. Report this log: the next step is a device at that "
                "path (the game folder, as TTT2 Web maps \\Device\\Cdrom0), not another sector answer.")
        else:
            recommendations.append(
                f"The game opened the bare device {', '.join(paths)} (the runtime's \"ResolvePath(\\Device) failed\" "
                "error) without asking for a disc sector first: it wants the device itself. Report this log: the next "
                "step is a device at that path (the game folder, as TTT2 Web maps \\Device\\Cdrom0).")
    if files["failed"]:
        other = [f for f in files["failed"] if f["path"] not in {d["path"] for d in files["device_opens"]}
                 and f["path"].lower() not in KNOWN_DEVICE_PROBES]
        if other:
            shown = ", ".join(f"{f['path']} ({f['status_name'] or f['status']})" for f in other[:5])
            recommendations.append(f"{len(other)} other path(s) failed to open, first: {shown}. A file of the game data "
                                   "that fails to open is missing from the extracted folder or mapped to the wrong place "
                                   "(game:, d:, update:, cache:).")
    if files["read_failures"]:
        r = files["read_failures"][0]
        recommendations.append(f"File reads failed ({len(files['read_failures'])} logged), first: {r['path']} at {r['offset']} "
                               f"length {r['length']} -> {r['status']}.")
    refused = [i for i in files["info"] if i["status"] in REFUSED_STATUSES]
    if refused:
        shown = "; ".join(f"{i['what']} class {i['class']} ({i['name']}) for {i['path']} -> {i['status']}"
                          f"{' ' + i['status_name'] if i['status_name'] else ''} from {i['caller']} on {i['thread']}"
                          for i in refused[:6])
        answered = sorted({i["class"] for i in refused if i["what"] == "query" and i["class"] in TTT2RECOMP_FILE_CLASSES})
        text = (f"The runtime refused {len(refused)} kind(s) of file information the game asked for or set: {shown}.")
        if answered:
            text += (f" TTT2Recomp, which runs the game to its menus, answers class(es) {', '.join(map(str, answered))} "
                     "(and any other class with success): a refused class the loader depends on can stop it before it "
                     "reads; compare with the reads logged after it.")
        recommendations.append(text)
    if (activity and files["reads"] and activity.get("reads", 0) <= len(files["reads"])
            and (stalls or ended_by_close)):
        last = files["reads"][-1]
        text = (f"The game read {activity.get('reads')} time(s) in all and every read is logged: the last one, "
                f"{last['path']} at {last['offset']} length {last['length']} ({last['bytes']} bytes, {last['status']}), "
                f"came from {last['caller']} on {last['thread']} at {last['time']}. The loader stopped after it")
        reader = re.search(r"id (\d+)\)?$", last["thread"])
        now = next((th for th in (last_dump or {}).get("threads", []) if reader and th["id"] == int(reader.group(1))), None)
        if now and now.get("wait"):
            text += f"; that thread, in the last dump: {now['wait']}"
            if now.get("stack"):
                text += f" (guest stack {now['stack']})"
            text += "."
        else:
            text += ": see what that thread waits on in the per-thread dump."
        recommendations.append(text)
    if activity:
        presented = activity.get("frames_presented", 0)
        if presented == 0:
            where = "before graphics initialisation (the GPU ring buffer was never set up)" \
                if activity.get("ring_buffer") == "not set up" else "after the GPU set-up but before its first frame"
            recommendations.append(
                f"No frame was presented (VdSwap was never called): the black window is the absence of any frame, not "
                f"black frames, and the game stopped {where}. The per-thread dump below says where each thread waits.")
        elif "frames_with_draws" in activity:
            with_draws = activity["frames_with_draws"]
            with_resolves = activity.get("frames_with_resolves", 0)
            followed, not_followed, method = frame_coverage(activity)
            if followed == 0:
                recommendations.append(
                    f"None of the {presented} presented frames could be followed in the GPU command stream"
                    + (f" ({not_followed} with VdSwap's buffer outside the primary ring buffer - in Direct3D's own command "
                       "buffer - which this host does not read)" if activity.get("swaps_outside_ring") else
                       f" ({not_followed} not followed)" if not_followed else "")
                    + ": what they asked the GPU to draw is unknown, not zero. Hosts that follow the ring's write pointer "
                    "(with the GPU emulation loaded) read every frame's commands, indirect buffers included.")
            elif with_draws == 0 and with_resolves == 0:
                if not_followed:
                    recommendations.append(
                        f"{followed} of {presented} frames were followed in the GPU command stream (by {method}) and not "
                        "one of them asked the GPU to draw: their commands hold no draw and no resolve (last frame: "
                        + activity.get("last_frame", "?") + f"). The other {not_followed} could not be followed; their "
                        "draws are unknown. Empty frames are a black screen of the game's own, as while it waits for its "
                        "data or a timer: what the game's threads wait for is the next question (see the dumps below).")
                else:
                    recommendations.append(
                        f"{presented} frames were presented and not one asked the GPU to draw: their command streams hold no "
                        "draw and no resolve (last frame: " + activity.get("last_frame", "?") + f"; read by {method}). The "
                        "game presents empty frames - a black screen of its own, as while it waits for its data or a timer - "
                        "so the GPU emulation is not what keeps the window black: what the game's threads wait for is (see "
                        "the dumps below).")
            else:
                recommendations.append(
                    f"The game draws: {activity.get('draws_total', '?')} draws and {activity.get('resolves_total', '?')} "
                    f"resolves ({activity.get('clears_total', '?')} clearing) in {with_draws} of {followed} frames "
                    f"followed (of {presented} presented), at most {activity.get('max_draws', '?')} in one. If the window "
                    "stays black, the question is what the frames contain: boot again with TTT2_PIXEL_CHECK=1 (the host "
                    "samples the presented front buffer, with the GPU emulation's readback of resolves turned on).")
            if activity.get("skipped_packets"):
                first = frames["skipped_packets"]
                recommendations.append(
                    f"The frames hold {activity['skipped_packets']} GPU packets the GPU emulation does not execute (it logs "
                    "them as \"Unimplemented GPU OPCODE\" and skips them)"
                    + (f", first in frame {first['frame']}: {first['stats']}" if first else "")
                    + ". If what they do matters to the picture, the GPU emulation needs them.")
        elif activity.get("frame_stats_off"):
            recommendations.append(f"{presented} frame(s) were presented; the frame monitor was off (TTT2_FRAME_STATS=0), "
                                   "so what they drew is unknown.")
        else:
            recommendations.append(
                f"{presented} frame(s) were presented (VdSwap). This host does not count what the frames draw; newer hosts "
                "log each frame's draws and resolves, which tells empty frames (the game's own black screen while it "
                "waits) from frames that are drawn but come out black (the GPU emulation).")
        for check in frames["pixel_checks"][-1:]:
            recommendations.append(f"Pixel check (frame {check['frame']}): {check['text']}.")
        if "kernel_clock_s" in activity and activity["kernel_clock_s"] == 0 and activity.get("tick_count_s", 0) >= 1:
            recommendations.append("The kernel clock (KeTimeStampBundle InterruptTime) stood still while TickCount moved: "
                                   "the host's 1 ms refresh is not running. Report this log.")
        elif activity.get("kernel_clock_state") == "off":
            recommendations.append("The kernel clock refresh was turned off (TTT2_KERNEL_CLOCK=runtime): the game's "
                                   "64-bit clocks stand still that way; unset it for a normal boot.")
        if activity.get("gpu_emulation") and activity.get("vblanks") == 0:
            recommendations.append("The GPU emulation's vblank counter never moved: its vsync thread is not running.")
    if last_dump:
        names = {t["id"]: t["name"] for t in last_dump["threads"]}
        cycles = find_deadlocks(last_dump["threads"])
        for cycle in cycles:
            chain = " -> ".join(f"{names.get(i, '?')} (id {i})" for i in cycle + cycle[:1])
            recommendations.append(f"Deadlock: each thread waits for a lock the next one owns: {chain}.")
        in_cycle = {i for c in cycles for i in c}
        by_id = {t["id"]: t for t in last_dump["threads"]}
        for th in last_dump["threads"]:
            # A chain that ends in a thread waiting on something without an owner (an event, a semaphore).
            if th["id"] in in_cycle or not owner_ids(th.get("wait")):
                continue
            path, node = [th["id"]], owner_ids(th.get("wait"))[0]
            while node in by_id and node not in path and owner_ids(by_id[node].get("wait")):
                path.append(node)
                node = owner_ids(by_id[node].get("wait"))[0]
            if node in path or node in in_cycle:
                continue
            path.append(node)
            end = by_id.get(node)
            chain = f"{names.get(path[0], '?')} (id {path[0]})" + "".join(
                f" waits for a lock held by {names.get(i, '?')} (id {i}), which" for i in path[1:])
            what = f" waits: {end['wait']}" if end and end.get("wait") else " is not in a traced wait"
            recommendations.append(f"Wait chain: {chain}{what}.")
        for th in last_dump["threads"]:
            busy = th.get("busy")
            if not busy and th.get("calls") and (mc := RE_CALLS_LAST_TICK.match(th["calls"])) \
                    and int(mc.group(1)) >= BUSY_CALLS_PER_TICK:
                busy = f"{mc.group(1)} traced kernel calls in one watchdog tick ({th['calls'].split(';')[0]})"
            if not busy:
                continue
            busy = busy.split(": a loop that keeps calling")[0]
            text = (f"{th['name']} (id {th['id']}) spins: {busy}. It keeps calling the kernel without waiting for long: "
                    "a polling loop, not a deadlock.")
            if activity is not None and "kernel_clock_s" not in activity and "kernel_clock_state" not in activity:
                text += (" This host predates the kernel clock fix: KeTimeStampBundle's InterruptTime and SystemTime stood "
                         "at 0, and TTT2 Web documents a game thread (sub_82ABFDE8) that signals its own event and waits on "
                         "it until a 64-bit counter changes, which spins forever on a clock that never moves. Boot again "
                         "with a current host.")
            elif activity is not None and activity.get("kernel_clock_state") == "off":
                text += " The kernel clock refresh was off (TTT2_KERNEL_CLOCK=runtime), which leaves such loops spinning."
            elif activity is not None and "kernel_clock_s" in activity:
                text += (f" The kernel clock advances ({activity['kernel_clock_s']} s), so the loop waits for something "
                         "else: the busy line names its call site and the guest stack the functions around it.")
            recommendations.append(text)
        # The critical sections a thread holds, from the host's own count of its enters and leaves (holds: lines).
        explained = set()
        for th in last_dump["threads"]:
            holds = th.get("holds") or []
            for text in holds:
                mh = RE_HOLD.match(text)
                if not mh:
                    continue
                address, depth = norm_hex(mh.group(1)), int(mh.group(2))
                waiters = [o for o in last_dump["threads"] if o is not th and o.get("wait")
                           and f"critical section {mh.group(1)}".lower() in o["wait"].lower()]
                if depth < 2:
                    continue  # an ordinary hold: the wait chain above already says who holds what
                stacks = [ms for ms in (RE_HOLD_STACK.match(x) for x in holds)
                          if ms and norm_hex(ms.group(1)) == address]
                rec = (f"Critical section {address} is held by {th['name']} (id {th['id']}) at depth {depth} since "
                       f"t+{mh.group(3)} s, unchanged for {mh.group(5)} s ({mh.group(6)} enters and {mh.group(7)} leaves "
                       f"since it was taken; the section's own count: {mh.group(4)})")
                if waiters:
                    rec += (f", while {len(waiters)} thread(s) wait for it: "
                            + ", ".join(f"{w['name']} (id {w['id']})" for w in waiters[:6]))
                rec += f". The enters still open came from {mh.group(8)}"
                if stacks:
                    deepest = stacks[-1]
                    rec += (f"; the enter that took it to depth {deepest.group(2)} (t+{deepest.group(3)} s) came through "
                            f"{deepest.group(4)}")
                rec += (". Those calls entered the section and their callers returned without leaving it; on the "
                        "console the code after them leaves it again. The functions on that stack are where to look.")
                recommendations.append(rec)
                explained.add(address)
        sections = {}
        for dump in [st for st in stalls] + ([close_dump] if close_dump else []):
            for th in dump["threads"]:
                for address, owner, recursion in RE_CS_WAIT.findall(th.get("wait") or ""):
                    seen = sections.setdefault(norm_hex(address), [])
                    if not seen or seen[-1][0] != dump["time"]:
                        seen.append((dump["time"], owner, int(recursion)))
        for address, seen in sections.items():
            if address in explained:
                continue
            owners = {owner for _, owner, _ in seen}
            counts = [recursion for _, _, recursion in seen]
            if len(owners) == 1 and len(set(counts)) > 1:
                recommendations.append(
                    f"Critical section {address} stayed with {seen[0][1]} while its recursion count went from {counts[0]} "
                    f"to {counts[-1]} between the dumps: its owner enters it again and again (RtlEnterCriticalSection) "
                    "and leaves it less often, so the threads waiting for it never get it. Its guest stack shows the loop.")
            elif max(counts) >= 64:
                recommendations.append(
                    f"Critical section {address} is held by {seen[-1][1]} with recursion count {max(counts)}: entered that "
                    "many times without being left. Nesting that deep is unusual; if the count changes between dumps, its "
                    "owner enters it in a loop.")
        blocked = [t for t in last_dump["threads"] if t.get("wait") and "no timeout" in t["wait"]
                   and ("not signaled" in t["wait"] or "never signaled" in t["wait"] or "count 0 of" in t["wait"])]
        if blocked:
            first = blocked[0]
            recommendations.append(
                f"{len(blocked)} thread(s) wait without a timeout on an event or semaphore nobody signals, e.g. "
                f"{first['name']} (id {first['id']}): {first['wait']}. Find what should signal it: the guest stacks name the "
                "functions; a missing interrupt (vblank), an I/O completion or a stubbed call are the usual causes.")
    if calls["cpp_exceptions"]:
        first = calls["cpp_exceptions"][0]
        n = (activity or {}).get("cpp_exceptions", len(calls["cpp_exceptions"]))
        recommendations.append(
            f"The game threw {n} C++ exception(s), the first ({first['type']}) from {first['from']} on {first['thread']}. "
            "The runtime cannot unwind the guest stack, so the code after each throw ran, which never happens on the "
            "console (TTT2 Web stops there): what came after it in this log is suspect. Why the game threw - a failed "
            "allocation, a missing file - is the next question.")
    for ex in calls["other_exceptions"][:1]:
        recommendations.append(f"The game raised exception {ex['code']} from {ex['from']} on {ex['thread']}: the runtime "
                               "does not handle it and goes on after the call.")
    if calls["debug_breaks"] or calls["bug_checks"]:
        hits = calls["bug_checks"] + calls["debug_breaks"]
        recommendations.append(
            f"The game stopped at {len(hits)} debug break(s) or bug check(s) (an assertion that failed), first from "
            f"{hits[0]['from']} on {hits[0]['thread']}: the runtime goes on after it; the console would not. The assertion "
            "names a state the game did not expect.")
    if calls["memory_failures"]:
        first = calls["memory_failures"][0]
        n = (activity or {}).get("memory_failures", len(calls["memory_failures"]))
        recommendations.append(f"{n} memory call(s) failed, first {first['call']} -> {first['status']} from {first['from']} "
                               f"on {first['thread']}: the game then runs a path the console does not.")
    if calls["apcs"]:
        first = calls["apcs"][0]
        n = (activity or {}).get("apcs_queued", len(calls["apcs"]))
        modes = sorted({(c["wait_mode"], c["alertable"]) for c in calls["signal_and_wait"]})
        recommendations.append(
            f"The game queued {n} user APC(s), the first to handle {first['handle']} (routine {first['routine']}). The "
            "runtime delivers them at the thread's next alertable wait; neither reference port delivers APCs at all."
            + (" Its signal-and-wait calls pass (wait mode, alertable) " + ", ".join(f"({m}, {a})" for m, a in modes)
               + ": the host passes the alertable flag where the runtime reads it." if modes else ""))
    unnamed = [n for n in calls["thread_names"] if n["applied"].startswith("no thread")]
    if unnamed:
        recommendations.append(f"{len(unnamed)} of the {len(calls['thread_names'])} thread names the game set reached no "
                               f"thread (first '{unnamed[0]['name']}' for thread id {unnamed[0]['id']}): the runtime found no "
                               "thread with the id the game passed, so the dumps cannot use those names.")
    if outcome.startswith("incomplete"):
        recommendations.append("The log has no end marker: if the host was still running, it was not frozen for 15 s "
                               "(no stall dump) and not crashed; attach the console output and what the window showed.")
    if too_few_cores:
        recommendations.append("The runtime warned 'Too few processor cores': guest thread scheduling is degraded on this "
                               "machine (informational; CI runners have 2-4 cores).")
    if not recommendations:
        recommendations.append("Nothing to fix from this log.")

    return {
        "outcome": outcome,
        "facts": {**facts, "arena": [hex(a) for a in facts["arena"]] if facts["arena"] else None},
        "timeline": {"first": first_ts, "last": last_ts, "duration_s": duration_s, "events": events,
                     "main_created": main_created, "main_exited": main_exited, "closed": closed, "exit_code": ended},
        "overrides": overrides,
        "watchdog": {"ticks": len(ticks), "last": ticks[-1] if ticks else None},
        "stalls": stalls,
        "close_dump": close_dump,
        "activity": activity,
        "traced_exports": traced_exports,
        "files": files,
        "milestones": milestones,
        "threads_created": threads_created,
        "kernel_clock": kernel_clock,
        "missing_names": missing_names,
        "frames": frames,
        "ended_by_close": ended_by_close,
        "stubs": {"source": stub_source, "total_calls": report["calls"] if report else sum(stub_warnings.values()),
                  "list": stubs},
        "crash": crash,
        "access_violations": {"total": sum(access_violations.values()),
                              "first": {"time": first_av[0], "operation": first_av[1], "guest": first_av[2]} if first_av else None},
        "variable_imports_not_implemented": var_imports,
        "invalid_calls": invalid_calls,
        "gpu_plugin_missing": gpu_missing,
        "errors": [{"message": first_error_text[k], "count": c} for k, c in errors.most_common(15)],
        "levels": dict(levels),
        "unparsed_lines": unparsed,
        "calls": calls,
        "recommendations": recommendations,
    }


def render_markdown(t, source):
    lines = [f"# Boot log triage", "", f"Source: `{source}`. Names, addresses and counts from the log; no game data.", "",
             f"## Outcome", "", f"**{t['outcome']}**", ""]
    f = t["facts"]
    tl = t["timeline"]
    lines += ["## Facts", "", "| Item | Value |", "|---|---|",
              f"| Executable | title {f['title_id'] or '?'} version {f['version'] or '?'} (base {f['base_version'] or '?'}), media {f['media_id'] or '?'}"
              + (" - **not TTT2**" if f["not_ttt2"] else "") + " |",
              f"| Graphics backend | {f['graphics_backend'] or '?'} |"]
    for k, v in f["roots"].items():
        lines.append(f"| {k} | `{v}` |")
    for lib, n in f["imports"].items():
        lines.append(f"| imports from {lib} | {n} |")
    lines += [f"| Log span | {tl['first'] or '?'} to {tl['last'] or '?'} ({tl['duration_s']} s) |",
              f"| Log lines by level | {', '.join(f'{k} {v}' for k, v in sorted(t['levels'].items()))} |",
              f"| Watchdog ticks | {t['watchdog']['ticks']} |", ""]
    if tl["events"]:
        lines += ["## Timeline", ""]
        for ts, text in tl["events"]:
            lines.append(f"* `{ts}` {text}")
        lines.append("")
    if t["watchdog"]["last"]:
        w = t["watchdog"]["last"]
        lines += ["## Last watchdog census", "",
                  f"`{w['time']}`: {w['threads']} guest threads ({w['running']} running); main thread "
                  f"{'running' if w['main_running'] else 'not running'}; threads: {w['names']}; kernel stubs: {w['stubs']}", ""]
    if t["crash"]:
        c = t["crash"]
        lines += ["## Host crash", "", f"* kind: {c['kind']}", f"* host pc: {c['host_pc']}"]
        for d in c["details"]:
            lines.append(f"* {d}")
        if c["exit_code"] is not None:
            lines.append(f"* exit code: {c['exit_code']}")
        lines.append("")
    a = t.get("activity")
    if a:
        lines += [f"## Boot progress (watchdog activity at `{a['time']}`)", "", "| Item | Value |", "|---|---|",
                  f"| Files opened / failed (bare devices) | {a.get('files_opened', '?')} / {a.get('open_failures', '?')} "
                  f"({a.get('device_failures', '?')}) |",
                  f"| Read | {a.get('mb_read', '?')} MB in {a.get('reads', '?')} reads ({a.get('read_failures', '?')} failed) |",
                  f"| Disc sector queries answered unsupported | {a.get('sector_queries', '?')} |",
                  f"| Threads created | {a.get('threads_created', '?')} |",
                  f"| GPU | engines {a.get('engines', '?')}, ring buffer {a.get('ring_buffer', '?')}, interrupt callback "
                  f"{a.get('interrupt_callback', '?')}"
                  + (f", vblanks {a['vblanks']}, ring write pointer {a.get('ring_write_pointer', '?')}" if "vblanks" in a
                     else ", no GPU emulation") + " |",
                  f"| Frames presented (VdSwap) | {a.get('frames_presented', '?')}"
                  + ((lambda c: f" ({c[0]} followed in the GPU command stream by {c[2]}"
                      + (f", {c[1]} not followed" if c[1] else "") + ")")(frame_coverage(a)) if "frames_with_draws" in a else "")
                  + (f": {a['frames_with_draws']} with draws, {a.get('frames_with_resolves', '?')} with resolves; "
                     f"{a.get('draws_total', '?')} draws, {a.get('resolves_total', '?')} resolves, "
                     f"{a.get('clears_total', '?')} clears; last frame: {a.get('last_frame', '?')}"
                     + (f"; front buffer {a['front_buffer']}" if a.get("front_buffer") else "")
                     if "frames_with_draws" in a else "") + " |",
                  f"| Audio | {a.get('audio_clients', '?')} client(s), {a.get('audio_frames', '?')} frames |",
                  f"| Input polls | {a.get('input_polls', '?')} |",
                  f"| Missing names answered as not found | {a.get('missing_names', '?')} |",
                  f"| Kernel clock (KeTimeStampBundle) | "
                  + (f"{a['kernel_clock_s']} s (tick count {a.get('tick_count_s', '?')} s)" if "kernel_clock_s" in a
                     else a.get("kernel_clock_state", "not reported (an older host: InterruptTime and SystemTime stood at 0)"))
                  + " |",
                  f"| Traced kernel calls | {a.get('traced_calls', '?')}"
                  + (f" (through {t['traced_exports']} wrapped exports)" if t.get("traced_exports") else "") + " |", ""]
    fs = t.get("files") or {}
    if (fs.get("failed") or fs.get("device_opens") or fs.get("opened") or fs.get("sector_first") or fs.get("reads")
            or fs.get("info") or fs.get("writes") or fs.get("dir_queries")):
        lines += ["## File access", ""]
        if fs.get("sector_first"):
            lines.append(f"* Disc sector queries answered as unsupported, first for `{fs['sector_first']['path']}` "
                         f"at `{fs['sector_first']['time']}`")
        for d in fs.get("device_opens", []):
            lines.append(f"* **Bare device opened**: `{d['path']}` at `{d['time']}` (failed)")
        for f in fs.get("failed", [])[:32]:
            lines.append(f"* open failed: `{f['path']}` -> {f['status']} {f['status_name'] or ''} ({f['api']}, {f['thread']}, "
                         f"from {f['caller']})")
        for f in fs.get("read_failures", [])[:16]:
            lines.append(f"* read failed: `{f['path']}` at {f['offset']} length {f['length']} -> {f['status']}")
        for f in fs.get("device_controls", [])[:16]:
            lines.append(f"* device control {f['code']} on `{f['path']}` -> {f['status']}")
        missing = fs.get("attributes_missing", [])
        if missing:
            lines.append(f"* {len(missing)} attribute queries for missing paths, first: "
                         + ", ".join(f"`{m['path']}`" for m in missing[:8]))
        if fs.get("opened"):
            lines.append(f"* {len(fs['opened'])} distinct paths opened (logged), first: "
                         + ", ".join(f"`{o['path']}`" for o in fs["opened"][:8]))
        for i in fs.get("info", [])[:48]:
            lines.append(f"* information {i['what']} class {i['class']} ({i['name']}) for `{i['path']}` -> {i['status']}"
                         f"{' ' + i['status_name'] if i['status_name'] else ''} from {i['caller']} on {i['thread']}")
        for r in fs.get("reads", [])[:32]:
            lines.append(f"* read {r['n']} at `{r['time']}`: `{r['path']}` at {r['offset']} length {r['length']} -> "
                         f"{r['status']}, {r['bytes']} bytes{'; ' + r['completion'] if r['completion'] else ''} "
                         f"from {r['caller']} on {r['thread']}")
        for w in fs.get("writes", [])[:16]:
            lines.append(f"* write{' failed' if w['failed'] else ''}: `{w['path']}` at {w['offset']} length {w['length']} "
                         f"-> {w['status']}, {w['bytes']} bytes from {w['caller']} on {w['thread']}")
        for d in fs.get("dir_queries", [])[:16]:
            lines.append(f"* directory query `{d['path']}` for `{d['pattern']}` -> {d['status']}"
                         f"{' ' + d['status_name'] if d['status_name'] else ''} from {d['caller']} on {d['thread']}")
        lines.append("")
    if t.get("milestones"):
        lines += ["## Milestones (GPU, audio, input, system dialogs)", ""]
        for m in t["milestones"][:60]:
            lines.append(f"* `{m['time']}` {m['area']}: {m['text']}")
        lines.append("")
    gc = t.get("calls") or {}
    if any(gc.get(k) for k in ("thread_names", "cpp_exceptions", "other_exceptions", "debug_breaks", "bug_checks",
                               "memory_failures", "apcs", "signal_and_wait")):
        lines += ["## The game's own calls (thread names, exceptions, debug breaks, APCs, memory)", ""]
        for n in gc.get("thread_names", [])[:40]:
            lines.append(f"* thread name '{n['name']}' for thread id {n['id']}: {n['applied']} (from {n['from']})")
        for e in gc.get("cpp_exceptions", [])[:16]:
            lines.append(f"* C++ exception {e['n']} ({e['type']}) from {e['from']} on {e['thread']}")
        for e in gc.get("other_exceptions", [])[:16]:
            lines.append(f"* exception {e['code']} from {e['from']} on {e['thread']}")
        for e in (gc.get("bug_checks", []) + gc.get("debug_breaks", []))[:16]:
            lines.append(f"* {'bug check ' + e['code'] if 'code' in e else 'debug break'} from {e['from']} on {e['thread']}")
        for e in gc.get("memory_failures", [])[:16]:
            lines.append(f"* memory: {e['call']} -> {e['status']} from {e['from']} on {e['thread']}")
        for e in gc.get("apcs", [])[:16]:
            lines.append(f"* APC queued to handle {e['handle']}: routine {e['routine']} -> {e['status']} from {e['from']}")
        for e in gc.get("signal_and_wait", [])[:16]:
            lines.append(f"* NtSignalAndWaitForSingleObjectEx from {e['from']}: wait mode {e['wait_mode']}, alertable "
                         f"{e['alertable']}")
        lines.append("")
    fr = t.get("frames") or {}
    if fr.get("first") or fr.get("first_with_draws") or fr.get("pixel_checks") or fr.get("front_buffers"):
        lines += ["## Frames (what the command streams asked the GPU to do)", ""]
        for f in fr.get("front_buffers", [])[:4]:
            lines.append(f"* front buffer {f['width']}x{f['height']} {f['format']} at {f['address']} (from frame {f['frame']})")
        for f in fr.get("first", []):
            lines.append(f"* frame {f['frame']}: {f['stats']} ({f['words']} words of commands)")
        if fr.get("first_with_draws"):
            lines.append(f"* first frame with draws: frame {fr['first_with_draws']['frame']}: {fr['first_with_draws']['stats']}")
        else:
            lines.append("* no frame with draws")
        if fr.get("skipped_packets"):
            lines.append(f"* packets the GPU emulation skips, first in frame {fr['skipped_packets']['frame']}: "
                         f"{fr['skipped_packets']['stats']}")
        for c in fr.get("pixel_checks", [])[:12]:
            lines.append(f"* pixel check, frame {c['frame']}: {c['text']}")
        lines.append("")
    if t.get("threads_created"):
        lines += ["## Threads created", ""]
        for c in t["threads_created"]:
            details = c["details"]
            lines.append(f"* `{c['time']}` id {c['id']}: start {c['start']}" + (f" ({c['function']})" if c["function"] else "")
                         + ((details if details.startswith(",") else " " + details) if details else ""))
        lines.append("")

    def dump_section(title, dump):
        out = [title, "", "| Thread | id | state | priority | suspend | wait_result |", "|---|---|---|---|---|---|"]
        for th in dump["threads"]:
            out.append(f"| {th['name']} | {th['id']} | {th['state']} | {th['priority']} | {th['suspend']} | {th['wait_result']} |")
        out.append("")
        detailed = [th for th in dump["threads"] if th.get("wait") or th.get("stack") or th.get("busy") or th.get("holds")
                    or th.get("sites")]
        for th in detailed:
            out.append(f"* **{th['name']} (id {th['id']})**")
            for key, label in (("wait", "wait"), ("calls", "kernel calls"), ("busy", "busy"), ("sites", "calls by site"),
                               ("stack", "guest stack")):
                if th.get(key):
                    out.append(f"  * {label}: {th[key]}")
            for obj in th.get("objects") or []:
                out.append(f"  * object: {obj}")
            for hold in th.get("holds") or []:
                out.append(f"  * holds: {hold}")
        if dump.get("busiest"):
            out.append(f"* traced kernel calls in the last tick: {dump['busiest']}")
        for f in dump.get("files", []):
            out.append(f"* recent file activity: {f}")
        if detailed or dump.get("busiest") or dump.get("files"):
            out.append("")
        return out

    for st in t["stalls"]:
        title = (f"## Still stalled at `{st['time']}` ({st['unchanged_ms']} ms unchanged)" if st.get("still")
                 else f"## Possible stall at `{st['time']}` ({st['unchanged_ms']} ms unchanged)")
        lines += dump_section(title, st)
    if t.get("close_dump"):
        lines += dump_section(f"## Thread state at {t['close_dump']['when']} (`{t['close_dump']['time']}`)", t["close_dump"])
    lines += [f"## Kernel stubs reached ({t['stubs']['source']}; {t['stubs']['total_calls']} calls)", ""]
    if t["stubs"]["list"]:
        lines += ["| Stub | Calls | First seen as call # | TTT2 Web (CC0) reference |", "|---|---|---|---|"]
        for s in t["stubs"]["list"]:
            lines.append(f"| {s['name']} | {s['calls']} | {s['first_seen'] if s['first_seen'] is not None else '-'} | "
                         f"{(s['ttt2web_kind'] + ' at ' + s['ttt2web']) if s['ttt2web'] else 'not implemented there either'} |")
    else:
        lines.append("None.")
    lines.append("")
    if t["overrides"]:
        lines += ["## Host kernel overrides reached", ""]
        for o in t["overrides"]:
            lines.append(f"* {o['name']} -> {o['result']} (first call at `{o['time']}`)")
        lines.append("")
    if t.get("invalid_calls"):
        lines += ["## Calls to unregistered functions (runtime trap)", ""]
        for c in t["invalid_calls"]:
            lines.append(f"* `{c['time']}` indirect call to {c['address']}")
        lines.append("")
    if t["access_violations"]["total"]:
        av = t["access_violations"]
        lines += ["## Unhandled guest access violations", "",
                  f"{av['total']} in total; first at `{av['first']['time']}`: {av['first']['operation']} of guest {av['first']['guest']}", ""]
    if t["variable_imports_not_implemented"]:
        lines += ["## Variable imports the runtime does not implement", ""]
        for v in t["variable_imports_not_implemented"]:
            lines.append(f"* {v['name']} ({v['library']} ordinal {v['ordinal']})")
        lines.append("")
    if t["errors"]:
        lines += ["## Errors and criticals (distinct, most frequent first)", ""]
        for e in t["errors"]:
            lines.append(f"* {e['count']}x `{e['message']}`")
        lines.append("")
    lines += ["## What to do next", ""]
    for i, r in enumerate(t["recommendations"], 1):
        lines.append(f"{i}. {r}")
    lines.append("")
    if t["unparsed_lines"]:
        lines += [f"({t['unparsed_lines']} line(s) did not match the log format and were ignored.)", ""]
    return "\n".join(lines)


def load_ttt2web_list(path):
    refs = {}
    if not path or not os.path.isfile(path):
        return refs
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) >= 2:
                refs[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "implemented")
    return refs


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="a log file, or the host's logs directory (newest session)")
    ap.add_argument("--md", help="write the Markdown report here")
    ap.add_argument("--json", help="write the structured triage here")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--ttt2web-list", default=os.path.join(here, "ttt2web_kernel_functions.txt"),
                    help="TTT2 Web reference list (name, file:line, kind per line)")
    ap.add_argument("--quiet", action="store_true", help="only print the outcome line")
    ap.add_argument("--closed-by", help="who closed the window when it was not a person, e.g. \"the boot step's 180 s "
                                        "timeout\" (scripts/run-host.ps1 passes it)")
    args = ap.parse_args(argv[1:])

    files = session_files(args.log)
    lines = []
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            lines.extend(fh.readlines())
    t = triage(lines, load_ttt2web_list(args.ttt2web_list), closed_by=args.closed_by)
    t["source_files"] = [os.path.basename(f) for f in files]
    md = render_markdown(t, args.log)
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(md)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(t, f, indent=2)
    if args.quiet:
        print(f"triage: {t['outcome']}")
    else:
        print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
