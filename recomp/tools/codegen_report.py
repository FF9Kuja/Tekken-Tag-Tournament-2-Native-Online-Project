#!/usr/bin/env python3
"""Summarises a `rexglue codegen` run as a report that contains no game bytes.

Reads the codegen output directory (the generated C++ and headers) and, optionally, the
log that `rexglue --log-level debug codegen ...` printed, and writes a Markdown report plus a
JSON file with counts and addresses only: functions, imports, jump tables, validation errors,
unimplemented instructions, unresolved calls. Addresses, names and counts are facts about the
executable (docs/LEGAL.md); the generated code itself never leaves the player's machine.

    python3 -I recomp/tools/codegen_report.py <generated dir> [--log codegen.log]
                                              [--project ttt2] [--md REPORT.md] [--json report.json]
                                              [--expect-functions N] [--expect-switches N] [--expect-imports N]
                                              [--fail-on-unimplemented] [--fail-on-errors]

Exit code 0 when every --expect/--fail-on condition holds (or none was given), 1 otherwise,
2 on usage errors. Formats parsed (ReXGlue SDK 0.10.0):
  * <project>_funcs.h          DECLARE_REX_FUNC(name);  one per function
  * <project>_init.cpp         PPCFuncMappings { 0xADDR, name } entries
  * <project>_recomp.N.cpp     DEFINE_REX_FUNC(name) bodies, one "\t// <disassembly>" comment per
                               translated instruction (counted into the instruction mix); markers:
        // UNIMPLEMENTED: <mnemonic>  + REX_UNIMPLEMENTED(0xADDR, "<mnemonic>")
        // ERROR: unresolved bl target 0xADDR
        // ERROR: conditional branch to unknown address 0xADDR
        __builtin_trap(); // ERROR - detected jump to null value
        switch (ctx.rN.u32) {        a recognised jump table (one per bctr)
        REX_FATAL("...")             fatal at run time (unresolved call / jump target)
  * log                        "=== ANALYSIS ERRORS ===", "<Category> (N):", "  0xA from 0xB: msg",
                               "Jump table detection failed at bctr 0x...", "Unimplemented: <m> at 0x...",
                               "Cannot resolve ordinal N from lib", "Unresolved b target 0x... from 0x..."
"""
import argparse
import collections
import json
import os
import re
import sys
from datetime import datetime, timezone

RE_DECLARE = re.compile(r"^DECLARE_REX_FUNC\((\w+)\);", re.M)
RE_MAPPING = re.compile(r"\{\s*(0x[0-9A-Fa-f]+),\s*(\w+)\s*\}")
RE_UNIMPL = re.compile(r'REX_UNIMPLEMENTED\((0x[0-9A-Fa-f]+),\s*"([^"]+)"\)')
RE_UNRESOLVED_BL = re.compile(r"// ERROR: unresolved bl target (0x[0-9A-Fa-f]+)")
RE_UNKNOWN_BC = re.compile(r"// ERROR: conditional branch to unknown address (0x[0-9A-Fa-f]+)")
RE_NULL_JUMP = re.compile(r"// ERROR - detected jump to null value")
RE_SWITCH = re.compile(r"^\tswitch \((?:ctx\.)?(r\d+)\.u32\) \{", re.M)
RE_CASE = re.compile(r"^\tcase (\d+):", re.M)
RE_FATAL = re.compile(r'REX_FATAL\("([^"]*)"\)')
RE_FUNC_DEF = re.compile(r"^(?:DEFINE_REX_FUNC|REX_FUNC|PPC_FUNC)\((\w+)\)", re.M)
# Every translated instruction is preceded by a comment with its disassembly: "\t// mnemonic operands".
RE_MNEMONIC = re.compile(r"^\t// ([a-z][a-z0-9_.]*)(?: |$)", re.M)

RE_LOG_CATEGORY = re.compile(r"(UnresolvedCall|MissingJumpTable|JumpTargetOutOfBounds|DiscontinuousFunction|UnimplementedInsn) \((\d+)\):")
RE_LOG_ERROR_ENTRY = re.compile(r"^\s*0x([0-9A-Fa-f]{8})(?: from 0x([0-9A-Fa-f]{8}))?: (.*)$")
RE_LOG_JT_FAIL = re.compile(r"Jump table detection failed at bctr 0x([0-9A-Fa-f]+)")
RE_LOG_UNIMPL = re.compile(r"Unimplemented: (\S+) at 0x([0-9A-Fa-f]+)")
RE_LOG_ORDINAL = re.compile(r"Cannot resolve ordinal (\d+) from (\S+)")
RE_LOG_UNRES_B = re.compile(r"Unresolved b target 0x([0-9A-Fa-f]+) from 0x([0-9A-Fa-f]+)")
RE_LOG_IMPORTS = re.compile(r"loaded (\d+) imports \((\d+) resolved, (\d+) unresolved, (\d+) variables skipped\)")
RE_LOG_SUMMARY = re.compile(r"Codegen summary: (\d+) written, (\d+) unchanged, (\d+) deleted")
RE_LOG_VALIDATION = re.compile(r"Code generation blocked: (\d+) validation errors")
RE_LOG_LEVEL = re.compile(r"\[(trace|debug|info|warning|warn|error|critical)\]", re.I)


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def find_project(gen_dir, project):
    if project:
        return project
    for name in sorted(os.listdir(gen_dir)):
        if name.endswith("_funcs.h") and "_funcs." not in name[:-8]:
            return name[: -len("_funcs.h")]
    return None


def analyse_generated(gen_dir, project):
    rep = {
        "project": project,
        "functions": 0,
        "function_names": [],
        "imports": 0,
        "import_names": [],
        "rexcrt": 0,
        "mappings": 0,
        "recomp_files": 0,
        "recomp_bytes": 0,
        "switches": [],
        "unimplemented": [],
        "unresolved_bl": [],
        "unknown_bc": [],
        "null_jumps": 0,
        "fatals": [],
        "definitions": 0,
        "mnemonics": {},
    }
    funcs_h = os.path.join(gen_dir, f"{project}_funcs.h")
    if os.path.isfile(funcs_h):
        names = RE_DECLARE.findall(read_text(funcs_h))
        rep["functions"] = len(names)
        rep["function_names"] = names
        rep["import_names"] = sorted(n for n in names if n.startswith("__imp__"))
        rep["imports"] = len(rep["import_names"])
        rep["rexcrt"] = sum(1 for n in names if n.startswith("rexcrt_"))
    init_cpp = os.path.join(gen_dir, f"{project}_init.cpp")
    if os.path.isfile(init_cpp):
        rep["mappings"] = len(RE_MAPPING.findall(read_text(init_cpp)))
    for name in sorted(os.listdir(gen_dir)):
        if not (name.startswith(f"{project}_recomp.") and name.endswith(".cpp")):
            continue
        path = os.path.join(gen_dir, name)
        text = read_text(path)
        rep["recomp_files"] += 1
        rep["recomp_bytes"] += os.path.getsize(path)
        rep["definitions"] += len(RE_FUNC_DEF.findall(text))
        for m in RE_MNEMONIC.findall(text):
            rep["mnemonics"][m] = rep["mnemonics"].get(m, 0) + 1
        for addr, mnemonic in RE_UNIMPL.findall(text):
            rep["unimplemented"].append({"address": addr.upper().replace("0X", "0x"), "mnemonic": mnemonic, "file": name})
        rep["unresolved_bl"] += [a for a in RE_UNRESOLVED_BL.findall(text)]
        rep["unknown_bc"] += [a for a in RE_UNKNOWN_BC.findall(text)]
        rep["null_jumps"] += len(RE_NULL_JUMP.findall(text))
        rep["fatals"] += RE_FATAL.findall(text)
        # Each `switch (rN.u32) {` is one recognised jump table; count its cases up to the
        # matching `default:`.
        for m in RE_SWITCH.finditer(text):
            end = text.find("\tdefault:", m.end())
            body = text[m.end(): end if end >= 0 else m.end()]
            rep["switches"].append({"register": m.group(1), "cases": len(RE_CASE.findall(body)), "file": name})
    return rep


def analyse_log(path):
    log = {
        "errors_by_category": {},
        "error_entries": [],
        "jump_table_failures": [],
        "unimplemented": [],
        "unresolved_ordinals": [],
        "unresolved_b": [],
        "imports": None,
        "summary": None,
        "blocked_validation_errors": None,
        "levels": collections.Counter(),
    }
    current = None
    for line in read_text(path).splitlines():
        lvl = RE_LOG_LEVEL.search(line)
        if lvl:
            log["levels"][lvl.group(1).lower().replace("warning", "warn")] += 1
        m = RE_LOG_CATEGORY.search(line)
        if m:
            current = m.group(1)
            log["errors_by_category"][current] = int(m.group(2))
            continue
        if current:
            # Entries follow the category header; strip any log prefix before the address.
            tail = line[line.find("  0x"):] if "  0x" in line else ""
            e = RE_LOG_ERROR_ENTRY.match(tail)
            if e:
                log["error_entries"].append({
                    "category": current,
                    "address": "0x" + e.group(1).upper(),
                    "from": ("0x" + e.group(2).upper()) if e.group(2) else None,
                    "message": e.group(3).strip(),
                })
                continue
            if "Total:" in line or not line.strip():
                current = None
        m = RE_LOG_JT_FAIL.search(line)
        if m:
            log["jump_table_failures"].append("0x" + m.group(1).upper())
        m = RE_LOG_UNIMPL.search(line)
        if m:
            log["unimplemented"].append({"mnemonic": m.group(1), "address": "0x" + m.group(2).upper()})
        m = RE_LOG_ORDINAL.search(line)
        if m:
            log["unresolved_ordinals"].append({"ordinal": int(m.group(1)), "library": m.group(2)})
        m = RE_LOG_UNRES_B.search(line)
        if m:
            log["unresolved_b"].append({"target": "0x" + m.group(1).upper(), "from": "0x" + m.group(2).upper()})
        m = RE_LOG_IMPORTS.search(line)
        if m:
            log["imports"] = {"total": int(m.group(1)), "resolved": int(m.group(2)),
                              "unresolved": int(m.group(3)), "variables": int(m.group(4))}
        m = RE_LOG_SUMMARY.search(line)
        if m:
            log["summary"] = {"written": int(m.group(1)), "unchanged": int(m.group(2)), "deleted": int(m.group(3))}
        m = RE_LOG_VALIDATION.search(line)
        if m:
            log["blocked_validation_errors"] = int(m.group(1))
    log["levels"] = dict(log["levels"])
    return log


def mnemonic_histogram(entries):
    hist = collections.Counter(e["mnemonic"] for e in entries)
    return [{"mnemonic": k, "count": v} for k, v in sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))]


def render_markdown(rep, log, checks):
    lines = []
    lines.append(f"# Codegen report: {rep['project']}")
    lines.append("")
    lines.append(f"Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} by recomp/tools/codegen_report.py. "
                 "Counts, addresses and names only; no game bytes.")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Item | Value |")
    lines.append("|---|---|")
    lines.append(f"| Functions declared | {rep['functions']} |")
    lines.append(f"| Function definitions in recomp.N.cpp | {rep['definitions']} |")
    lines.append(f"| Kernel/XAM imports (`__imp__`) | {rep['imports']} |")
    lines.append(f"| rexcrt redirects | {rep['rexcrt']} |")
    lines.append(f"| Function table entries | {rep['mappings']} |")
    lines.append(f"| Generated translation units | {rep['recomp_files']} ({rep['recomp_bytes'] / (1024 * 1024):.1f} MiB) |")
    lines.append(f"| Jump tables recognised (switch statements) | {len(rep['switches'])} |")
    lines.append(f"| Unimplemented instructions (sites) | {len(rep['unimplemented'])} |")
    lines.append(f"| Unresolved `bl` targets | {len(rep['unresolved_bl'])} |")
    lines.append(f"| Conditional branches to unknown addresses | {len(rep['unknown_bc'])} |")
    lines.append(f"| Null jump-table entries | {rep['null_jumps']} |")
    lines.append(f"| Run-time fatals emitted | {len(rep['fatals'])} |")
    if log:
        imp = log.get("imports")
        if imp:
            lines.append(f"| Imports (log) | {imp['total']} total, {imp['resolved']} resolved, {imp['unresolved']} unresolved, {imp['variables']} variables |")
        if log.get("blocked_validation_errors") is not None:
            lines.append(f"| Validation errors blocking codegen | {log['blocked_validation_errors']} |")
        lines.append(f"| Jump-table detection failures (log) | {len(log['jump_table_failures'])} |")
        lines.append(f"| Unresolved import ordinals (log) | {len(log['unresolved_ordinals'])} |")
        if log.get("summary"):
            s = log["summary"]
            lines.append(f"| Files written / unchanged / deleted | {s['written']} / {s['unchanged']} / {s['deleted']} |")
        if log.get("levels"):
            lv = log["levels"]
            lines.append(f"| Log lines (warn / error) | {lv.get('warn', 0)} / {lv.get('error', 0)} |")
    lines.append("")
    if rep["unimplemented"]:
        lines.append("## Unimplemented instructions")
        lines.append("")
        lines.append("| Mnemonic | Sites |")
        lines.append("|---|---|")
        for h in mnemonic_histogram(rep["unimplemented"]):
            lines.append(f"| `{h['mnemonic']}` | {h['count']} |")
        lines.append("")
        lines.append("Addresses (first 50):")
        for e in rep["unimplemented"][:50]:
            lines.append(f"* {e['address']} `{e['mnemonic']}` ({e['file']})")
        lines.append("")
    if log and log["errors_by_category"]:
        lines.append("## Analysis errors (from the log)")
        lines.append("")
        for cat, n in sorted(log["errors_by_category"].items()):
            lines.append(f"* **{cat}**: {n}")
        lines.append("")
        lines.append("First 50 entries:")
        for e in log["error_entries"][:50]:
            src = f" from {e['from']}" if e["from"] else ""
            lines.append(f"* {e['category']} {e['address']}{src}: {e['message']}")
        lines.append("")
    if log and log["jump_table_failures"]:
        lines.append("## Jump-table detection failures")
        lines.append("")
        lines.append("Each is a `bctr` with an indexed load and a `lis` the detector could not complete. "
                     "Candidates for `[[switch_tables]]` entries (recomp/tools/find_switch_tables.py).")
        for a in log["jump_table_failures"][:100]:
            lines.append(f"* {a}")
        lines.append("")
    if rep["unresolved_bl"] or rep["unknown_bc"]:
        lines.append("## Unresolved branch targets in the generated code")
        lines.append("")
        for a in rep["unresolved_bl"][:50]:
            lines.append(f"* bl -> {a}")
        for a in rep["unknown_bc"][:50]:
            lines.append(f"* bc -> {a}")
        lines.append("")
    if rep["switches"]:
        sizes = collections.Counter(s["cases"] for s in rep["switches"])
        lines.append("## Jump tables")
        lines.append("")
        lines.append("Case counts: " + ", ".join(f"{n} x{c}" for n, c in sorted(sizes.items())))
        lines.append("")
    if rep["mnemonics"]:
        lines.append("## Instruction mix")
        lines.append("")
        lines.append(f"{sum(rep['mnemonics'].values())} translated instructions, {len(rep['mnemonics'])} distinct mnemonics. "
                     "Most frequent:")
        lines.append("")
        top = sorted(rep["mnemonics"].items(), key=lambda kv: (-kv[1], kv[0]))[:40]
        lines.append(", ".join(f"`{m}` {n}" for m, n in top))
        lines.append("")
    if checks:
        lines.append("## Checks")
        lines.append("")
        for ok, text in checks:
            lines.append(f"* {'PASS' if ok else 'FAIL'}: {text}")
        lines.append("")
    return "\n".join(lines) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("generated_dir")
    ap.add_argument("--log")
    ap.add_argument("--project")
    ap.add_argument("--md")
    ap.add_argument("--json")
    ap.add_argument("--expect-functions", type=int)
    ap.add_argument("--expect-switches", type=int)
    ap.add_argument("--expect-unimplemented", type=int, help="exact number of unimplemented sites expected")
    ap.add_argument("--expect-imports", type=int, help="exact number of resolved kernel/XAM imports (__imp__) expected")
    ap.add_argument("--expect-mnemonics", metavar="FILE",
                    help="file with one mnemonic per line (# comments allowed); each must occur in the generated code")
    ap.add_argument("--absent-ok", default="",
                    help="comma-separated mnemonics from --expect-mnemonics that may be absent (known gaps)")
    ap.add_argument("--fail-on-unimplemented", action="store_true")
    ap.add_argument("--fail-on-errors", action="store_true",
                    help="fail on unresolved calls, analysis errors or jump-table detection failures")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv[1:])

    if not os.path.isdir(args.generated_dir):
        print(f"not a directory: {args.generated_dir}", file=sys.stderr)
        return 2
    project = find_project(args.generated_dir, args.project)
    if not project:
        print(f"no <project>_funcs.h in {args.generated_dir} (did codegen run?)", file=sys.stderr)
        return 2
    rep = analyse_generated(args.generated_dir, project)
    log = analyse_log(args.log) if args.log and os.path.isfile(args.log) else None

    checks = []
    if args.expect_functions is not None:
        checks.append((rep["functions"] == args.expect_functions,
                       f"{rep['functions']} functions declared (expected {args.expect_functions})"))
    if args.expect_switches is not None:
        checks.append((len(rep["switches"]) == args.expect_switches,
                       f"{len(rep['switches'])} jump tables recognised (expected {args.expect_switches})"))
    if args.expect_unimplemented is not None:
        checks.append((len(rep["unimplemented"]) == args.expect_unimplemented,
                       f"{len(rep['unimplemented'])} unimplemented sites (expected {args.expect_unimplemented})"))
    if args.expect_imports is not None:
        checks.append((rep["imports"] == args.expect_imports,
                       f"{rep['imports']} kernel/XAM imports resolved (expected {args.expect_imports})"))
    if args.expect_mnemonics:
        with open(args.expect_mnemonics, encoding="utf-8") as f:
            wanted = [ln.split("#", 1)[0].strip().lower() for ln in f if ln.split("#", 1)[0].strip()]
        tolerated = {m.strip().lower() for m in args.absent_ok.split(",") if m.strip()}
        absent = [m for m in wanted if m not in rep["mnemonics"]]
        unexpected = [m for m in absent if m not in tolerated]
        checks.append((not unexpected, f"{len(wanted) - len(absent)}/{len(wanted)} listed mnemonics translated"
                       + (f"; absent: {', '.join(absent)}" if absent else "")
                       + (f" (tolerated: {', '.join(sorted(tolerated & set(absent)))})" if tolerated & set(absent) else "")))
    if args.fail_on_unimplemented:
        checks.append((not rep["unimplemented"], f"{len(rep['unimplemented'])} unimplemented instruction sites"))
    if args.fail_on_errors:
        n_err = len(rep["unresolved_bl"]) + len(rep["unknown_bc"]) + len(rep["fatals"])
        if log:
            n_err += sum(log["errors_by_category"].values()) + len(log["jump_table_failures"]) + len(log["unresolved_ordinals"])
        checks.append((n_err == 0, f"{n_err} error markers / analysis errors"))

    result = {"generated": {k: v for k, v in rep.items() if k != "function_names"}, "log": log,
              "checks": [{"ok": ok, "text": t} for ok, t in checks]}
    result["generated"]["unimplemented_histogram"] = mnemonic_histogram(rep["unimplemented"])
    md = render_markdown(rep, log, checks)
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(md)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    if not args.quiet:
        sys.stdout.write(md)
    return 0 if all(ok for ok, _ in checks) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
