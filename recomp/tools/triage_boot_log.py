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
RE_STALL = re.compile(r"ttt2: possible stall: thread census and stub calls unchanged for (\d+) ms")
RE_STALL_THREAD = re.compile(r"ttt2:\s+(\S.*?)\s+id (\d+)\s+(running|stopped) priority (-?\d+) suspend (\d+) wait_result (0x[0-9a-fA-F]+)")
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


def triage(lines, ttt2web=None):
    """Returns the structured triage of the given log lines (dicts and lists only)."""
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
    errors = collections.Counter()
    first_error_text = {}
    levels = collections.Counter()
    first_ts = last_ts = None
    main_created = main_exited = ended = closed = None
    too_few_cores = False
    current_stall = None
    unparsed = 0

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
        elif (mm := RE_ENDED.search(msg)):
            ended = int(mm.group(1))
            events.append((ts, f"host exited with code {ended} after the program ended"))
        elif (mm := RE_OVERRIDE.search(msg)):
            overrides.append({"name": mm.group(1), "result": mm.group(2), "time": ts})
        elif (mm := RE_WATCHDOG.search(msg)):
            ticks.append({"time": ts, "threads": int(mm.group(1)), "running": int(mm.group(2)),
                          "main_running": mm.group(3) == "running", "names": mm.group(4), "stubs": mm.group(5)})
            current_stall = None
        elif (mm := RE_STALL.search(msg)):
            current_stall = {"time": ts, "unchanged_ms": int(mm.group(1)), "threads": []}
            stalls.append(current_stall)
            events.append((ts, f"possible stall: nothing changed for {mm.group(1)} ms"))
        elif current_stall is not None and (mm := RE_STALL_THREAD.search(msg)):
            current_stall["threads"].append({"name": mm.group(1), "id": int(mm.group(2)), "state": mm.group(3),
                                             "priority": int(mm.group(4)), "suspend": int(mm.group(5)),
                                             "wait_result": mm.group(6)})
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
        elif "Window closing, shutting down" in msg:
            closed = ts
            events.append((ts, "window closed by the user"))
        elif "Too few processor cores" in msg:
            too_few_cores = True

    # Outcome.
    if crash:
        outcome = f"crash: {crash['kind']}"
    elif ended is not None:
        outcome = "clean exit: the program returned from its entry point and the host ended"
    elif stalls:
        # The owner's most likely first boot: a black or frozen window, the stall dump, then the X button.
        outcome = "stall: the thread census and stub calls stopped changing while the main thread ran"
        if closed:
            outcome += "; the window was closed afterwards"
    elif closed:
        outcome = "closed by the user while the game was running"
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
        waiting = [t for t in st["threads"] if t["state"] != "running"]
        recommendations.append(f"Stall after {st['unchanged_ms']} ms without change: {len(st['threads'])} guest threads, "
                               f"{len(waiting)} not running. Look at what the running threads wait on (events, vblank, a "
                               "stubbed call that never signals) in the per-thread dump.")
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
        "stubs": {"source": stub_source, "total_calls": report["calls"] if report else sum(stub_warnings.values()),
                  "list": stubs},
        "crash": crash,
        "access_violations": {"total": sum(access_violations.values()),
                              "first": {"time": first_av[0], "operation": first_av[1], "guest": first_av[2]} if first_av else None},
        "variable_imports_not_implemented": var_imports,
        "gpu_plugin_missing": gpu_missing,
        "errors": [{"message": first_error_text[k], "count": c} for k, c in errors.most_common(15)],
        "levels": dict(levels),
        "unparsed_lines": unparsed,
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
    for st in t["stalls"]:
        lines += [f"## Possible stall at `{st['time']}` ({st['unchanged_ms']} ms unchanged)", "",
                  "| Thread | id | state | priority | suspend | wait_result |", "|---|---|---|---|---|---|"]
        for th in st["threads"]:
            lines.append(f"| {th['name']} | {th['id']} | {th['state']} | {th['priority']} | {th['suspend']} | {th['wait_result']} |")
        lines.append("")
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
    args = ap.parse_args(argv[1:])

    files = session_files(args.log)
    lines = []
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            lines.extend(fh.readlines())
    t = triage(lines, load_ttt2web_list(args.ttt2web_list))
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
