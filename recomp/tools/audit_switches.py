#!/usr/bin/env python3
"""Audits how ReXGlue emitted every `bctr` of the image, against recomp/tools/find_switch_tables.py.

ReXGlue turns a `bctr` into a C++ `switch` when it has a jump table for it: one of ours
(`[[switch_tables]]`, written by the finder), or one its own detector found. Every other `bctr` becomes
an indirect call through the function table (REX_CALL_INDIRECT_FUNC), which is right for a tail call
through a pointer and traps at run time ("Call to invalid or unregistered function") when the target
is a label inside a function. The SDK's detector was measured to switch on the wrong register and to
misjudge table lengths (docs/RESEARCH.md 8a), so a switch that only the SDK found is checked with the
finder's model of the dispatch code: a certain miscompile when the switch tests a value read from the
table or a case goes elsewhere than the table entry for its number, consistent when every case matches.

This tool reads the generated sources, rebuilds the address of every instruction (ReXGlue writes one
`// <mnemonic>` comment per instruction, blocks in address order, a `loc_XXXXXXXX:` label at each
branch target) and checks each `bctr` against the image. It reports:
  * the finder's tables: the generated switch must test the same register with the same labels;
  * switches only the SDK found, with that verdict;
  * the dispatches the finder could not read: how ReXGlue emitted each (an indirect call traps if the
    game reaches it), with the finder's reason and the disassembled dispatch code from its JSON.

    python3 -I recomp/tools/audit_switches.py --image recomp/game/default.xex --generated recomp/generated/default
                                             --finder <switch_tables.json> [--md REPORT.md] [--json report.json]

Exit code 0 when the audit ran (whatever it found; the report says), 1 when the generated code contradicts
the finder's tables, 2 on input errors. Addresses and mnemonics only; the report stays local.
"""
import argparse
import glob
import importlib.util
import json
import os
import re
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("find_switch_tables", os.path.join(_HERE, "find_switch_tables.py"))
fst = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(fst)

RE_REGISTER = re.compile(r"SetFunction\(0x([0-9A-Fa-f]+),\s*(\w+)\)")
RE_DEFINE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
RE_LABEL = re.compile(r"^loc_([0-9A-Fa-f]+):")
RE_INSN = re.compile(r"^\t// ([a-z][a-z0-9_.+-]*|\.[a-z]+)(?: |$)")
RE_SWITCH = re.compile(r"^\tswitch \(ctx\.r(\d+)\.u32\) \{")
RE_CASE = re.compile(r"^\tcase (\d+):")
RE_GOTO = re.compile(r"^\t\tgoto loc_([0-9A-Fa-f]+);")
RE_CALL = re.compile(r"^\t\t(\w+)\(ctx, base\);")
RE_FATAL = re.compile(r"Jump target 0x([0-9A-Fa-f]+)")


def load_image(path):
    with open(path, "rb") as f:
        data = f.read()
    pe, base = fst.parse_xex2(data)
    return pe, base


def parse_generated(gen_dir, word):
    """{bctr: {"form": "switch"|"indirect", "register", "cases": [target], "function"}}, problems."""
    names = {}
    for path in glob.glob(os.path.join(gen_dir, "*_register.cpp")):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in RE_REGISTER.finditer(f.read()):
                names[m.group(2)] = int(m.group(1), 16)
    sites, problems = {}, []
    files = sorted(glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp")),
                   key=lambda p: int(re.search(r"\.(\d+)\.cpp$", p).group(1)) if re.search(r"\.(\d+)\.cpp$", p) else 0)
    for path in files:
        func, pc, pending = None, None, None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if pending is not None and line.strip():
                    site = pending
                    m = RE_SWITCH.match(line)
                    if m and site["form"] is None:
                        site.update(form="switch", register=int(m.group(1)))
                        continue
                    if site["form"] is None:
                        site["form"] = "indirect" if "REX_CALL_INDIRECT_FUNC" in line else "other"
                        pending = None
                        continue
                    m = RE_CASE.match(line)
                    if m:
                        site["cases"].append(None)
                        continue
                    if site["cases"] and site["cases"][-1] is None:
                        g, c, x = RE_GOTO.match(line), RE_CALL.match(line), RE_FATAL.search(line)
                        if g:
                            site["cases"][-1] = int(g.group(1), 16)
                        elif c:
                            site["cases"][-1] = names.get(c.group(1), c.group(1))
                        elif x:
                            site["cases"][-1] = ("unresolved", int(x.group(1), 16))
                        elif "__builtin_trap" in line:
                            site["cases"][-1] = ("null", 0)
                        continue
                    if line.startswith("\tdefault:") or line.startswith("\t}"):
                        if line.startswith("\t}"):
                            pending = None
                        continue
                m = RE_DEFINE.match(line)
                if m:
                    func = m.group(1)
                    pc = names.get(func)
                    if pc is None and re.fullmatch(r"sub_[0-9A-Fa-f]{8}", func):
                        pc = int(func[4:], 16)
                    continue
                m = RE_LABEL.match(line)
                if m:
                    pc = int(m.group(1), 16)
                    continue
                m = RE_INSN.match(line)
                if m and pc is not None:
                    if m.group(1) == "bctr":
                        if word(pc) != fst.BCTR:
                            problems.append(f"{os.path.basename(path)}: `// bctr` in {func} maps to 0x{pc:08X}, "
                                            f"which does not hold a bctr")
                        else:
                            pending = sites[pc] = {"form": None, "register": None, "cases": [], "function": func}
                    pc += 4
    return sites, problems


def sdk_verdict(finder, bctr, site):
    """Checks a switch only the SDK found with the finder's model of the dispatch code."""
    st = finder.emulate(finder.region_start(bctr), bctr)
    t, reg = st.ctr, st.regs[site["register"]]
    if t is not None and t[0] in ("t", "n") and (reg == t or reg[0] in ("t", "n")):
        return "miscompile: the switch tests a value read from the jump table, not the index"
    if t is None or t[0] not in ("t", "n") or reg[0] != "l":
        return "unverified: the finder's model cannot follow this dispatch"
    inner = t[1] if t[0] == "n" else t
    _, size, signed, sid, k, a0, shift, base = inner
    if reg[1] != sid:
        return "suspect: the switch register does not hold the value that indexes the table"
    if reg[2]:
        return "unverified: the switch register holds a scaled index"
    for n, target in enumerate(site["cases"]):
        p = (a0 + ((n - reg[3]) << k)) & fst.M32
        raw = finder.read(p, size, signed)
        if raw is None:
            return f"suspect: case {n} reads 0x{p:08X}, outside the image"
        if t[0] == "n":
            label = finder.read((base + (raw << shift)) & fst.M32, 4)
        elif base is None:
            label = raw & fst.M32 if size == 4 and not shift else None
        else:
            label = (base + (raw << shift)) & fst.M32
        if isinstance(target, int) and label != target:
            return (f"miscompile: case {n} goes to 0x{target:08X}, but the table entry for {n} is "
                    + (f"0x{label:08X}" if label is not None else "not an address"))
    return "consistent: every case is the table entry for its number (the length is the SDK's guess)"


def audit(image_path, gen_dir, finder_path):
    pe, base = load_image(image_path)
    with open(finder_path, encoding="utf-8") as f:
        finder = json.load(f)
    return audit_image(fst.Finder(pe, base, fst.pe_sections(pe, base)), gen_dir, finder)


def audit_image(finder_model, gen_dir, finder):
    """The audit of one image (a find_switch_tables.Finder) against the finder's JSON report."""
    word = finder_model.word
    tables = {int(t["bctr"], 16): t for t in finder.get("tables", [])}
    unresolved = {int(u["bctr"], 16): u for u in finder.get("unresolved_sites", [])}
    unbounded = {int(u["bctr"], 16): u for u in finder.get("unbounded_sites", [])}
    sites, problems = parse_generated(gen_dir, word)

    mismatches, sdk_only, finder_unresolved = [], [], []
    for bctr, t in sorted(tables.items()):
        site = sites.get(bctr)
        labels = [int(x, 16) for x in t["labels"]]
        if site is None:
            mismatches.append({"bctr": bctr, "problem": "not found in the generated code"})
        elif site["form"] != "switch":
            mismatches.append({"bctr": bctr, "problem": f"emitted as {site['form']}, not as a switch"})
        elif site["register"] != t["register"] or site["cases"] != labels:
            mismatches.append({"bctr": bctr, "problem": f"switch on r{site['register']} with {len(site['cases'])} cases; "
                                                        f"the finder's table tests r{t['register']} with {len(labels)} labels"})
    for bctr, site in sorted(sites.items()):
        if bctr in tables or site["form"] != "switch":
            continue
        verdict = sdk_verdict(finder_model, bctr, site)
        if bctr in unresolved and "another path enters" in unresolved[bctr]["reason"] and not verdict.startswith("miscompile"):
            verdict = ("suspect: " + verdict.split(": ", 1)[-1] + "; but another path enters the dispatch "
                       "and reads a different entry there (see the finder's reason)")
        entry = {"bctr": bctr, "function": site["function"], "register": site["register"],
                 "cases": len(site["cases"]), "verdict": verdict,
                 "finder": ("unresolved: " + unresolved[bctr]["reason"]) if bctr in unresolved else
                           "a table read without a bounds check" if bctr in unbounded else "no table"}
        if bctr in unresolved:
            entry["sequence"] = unresolved[bctr].get("sequence", [])
        sdk_only.append(entry)
    for bctr, u in sorted(unresolved.items()):
        site = sites.get(bctr)
        form = site["form"] if site else "absent"
        finder_unresolved.append({"bctr": bctr, "function": site["function"] if site else None, "form": form,
                                  "effect": {"indirect": "traps if the game reaches it (no registered function there)",
                                             "switch": "a switch from the SDK's detector (verdict above)",
                                             "absent": "not in the generated code (unreachable for ReXGlue)"}.get(form, form),
                                  "reason": u["reason"], "sequence": u.get("sequence", [])})
    counts = {"bctr_in_generated_code": len(sites),
              "finder_tables": len(tables),
              "finder_tables_matching": len(tables) - len(mismatches),
              "sdk_only_switches": len(sdk_only),
              "sdk_only_miscompiles": sum(1 for e in sdk_only if e["verdict"].startswith("miscompile")),
              "sdk_only_consistent": sum(1 for e in sdk_only if e["verdict"].startswith("consistent")),
              "indirect": sum(1 for e in sites.values() if e["form"] == "indirect"),
              "finder_unresolved": len(unresolved),
              "finder_unresolved_indirect": sum(1 for u in finder_unresolved if u["form"] == "indirect"),
              "mapping_problems": len(problems)}
    return {"counts": counts, "mismatches": mismatches, "sdk_only": sdk_only,
            "finder_unresolved": finder_unresolved, "problems": problems[:50]}


def _jsonable(v, key=None):
    if isinstance(v, dict):
        return {k: _jsonable(x, k) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_jsonable(x, key) for x in v]
    if isinstance(v, int) and not isinstance(v, bool) and key in ("bctr", "cases"):
        return f"0x{v:08X}"
    return v


def markdown(res):
    c = res["counts"]
    out = ["# Switch dispatch audit (recomp/tools/audit_switches.py)", "",
           "How ReXGlue emitted each `bctr` of the image, against the switch-table finder. Local diagnostics: "
           "addresses and mnemonics only.", "",
           "| | |", "|---|---|",
           f"| `bctr` sites in the generated code | {c['bctr_in_generated_code']} |",
           f"| Finder tables emitted as the finder wrote them | {c['finder_tables_matching']} of {c['finder_tables']} |",
           f"| Switches only the SDK's detector found | {c['sdk_only_switches']} ({c['sdk_only_miscompiles']} certain miscompiles, "
           f"{c['sdk_only_consistent']} consistent with their table) |",
           f"| Indirect calls through the function table | {c['indirect']} |",
           f"| Dispatches the finder could not read | {c['finder_unresolved']} ({c['finder_unresolved_indirect']} emitted as indirect calls) |",
           f"| Address-mapping problems | {c['mapping_problems']} |", ""]
    if res["mismatches"]:
        out += ["## The generated code contradicts the finder's tables", ""]
        out += [f"- 0x{m['bctr']:08X}: {m['problem']}" for m in res["mismatches"]] + [""]
    if res["sdk_only"]:
        out += ["## Switches only the SDK's detector found", "",
                "| bctr | function | switch register | cases | verdict | finder |", "|---|---|---|---|---|---|"]
        out += [f"| 0x{s['bctr']:08X} | {s['function']} | r{s['register']} | {s['cases']} | {s['verdict']} | {s['finder']} |"
                for s in res["sdk_only"]] + [""]
    if res["finder_unresolved"]:
        out += ["## Dispatches the finder could not read", ""]
        for u in res["finder_unresolved"]:
            out += [f"### 0x{u['bctr']:08X} in {u['function']}: {u['form']}", "", f"- Effect: {u['effect']}",
                    f"- Finder: {u['reason']}", ""]
            if u["sequence"]:
                out += ["```"] + u["sequence"] + ["```", ""]
    if res["problems"]:
        out += ["## Address-mapping problems", ""] + [f"- {p}" for p in res["problems"]] + [""]
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--generated", required=True)
    ap.add_argument("--finder", required=True, help="the JSON written by find_switch_tables.py --json")
    ap.add_argument("--md")
    ap.add_argument("--json")
    args = ap.parse_args(argv[1:])
    try:
        res = audit(args.image, args.generated, args.finder)
    except (OSError, ValueError, KeyError, struct.error, json.JSONDecodeError) as e:
        print(f"audit_switches: error: {e}", file=sys.stderr)
        return 2
    c = res["counts"]
    print(f"audit_switches: {c['bctr_in_generated_code']} bctr sites; {c['finder_tables_matching']} of {c['finder_tables']} "
          f"finder tables emitted as written; {c['sdk_only_switches']} SDK-only switches ({c['sdk_only_miscompiles']} certain "
          f"miscompiles); {c['finder_unresolved']} unread dispatches ({c['finder_unresolved_indirect']} indirect calls that "
          f"trap if reached); {c['mapping_problems']} mapping problems")
    if args.md:
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(markdown(res))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(_jsonable(res), f, indent=2)
    return 1 if res["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
