#!/usr/bin/env python3
"""Resolves ReXGlue's "target not in any function" errors with verified [functions] entries.

Why these errors happen (ReXGlue 0.10.0, pin c94f5eb and upstream bd833a2 alike): the analysis
registers a function for every .pdata entry, `bl` target, RTTI vtable slot, lis/addi code pointer,
[functions] config entry and gap-fill run, but never for the target of a plain `b`. A leaf function
that is only entered by a tail branch (Tekken Tag Tournament 2 has hundreds: adjustor thunks such as
`addi r3,r3,-N; b method`) therefore exists only if gap fill covers it, and gap fill skips any run of
code that starts at a known function, including the code after that function's last unconditional
branch. The same gap fill also cuts a block that sits after a function's blr (rarely taken paths)
into a function of its own when the real function was never registered; that block's branch back
into the body then has no function to land in. Validation stops codegen on both (recomp/tests/ppc/
tailcall_ttt2.s reproduces them; docs/RESEARCH.md has the measurements on the owner's image).

What this tool does with each UnresolvedCall error `b T from S`:
  1. reads the instruction at S in the image: it must be a direct branch to T (else the log does not
     belong to this image, and nothing is added);
  2. looks T up in a boundary reference: TTT2 Web's control-flow-derived table (CC0, recomp/
     ttt2_functions.ttt2web.toml), accepted only when the eight save/restore helper addresses ReXGlue
     itself found in this run equal the ones published with it, plus the image's own .pdata;
  3. adds an entry only when the two agree:
       entry     T starts a reference function: "0xT" = {} (the entry point only; ReXGlue's own
                 discovery finds the extent, as for every other function it registers),
       function  S and T both lie inside one reference function F: "0xF" = { size = ... } (F's extent
                 from the reference, so the branch is internal again);
     anything else is reported as unverified and nothing is guessed.
The entries go to a file the manifest includes (recomp/game/ttt2_resolved_functions.toml, git-ignored
with the image; addresses only), stamped with the image's SHA-1 so they are never applied to another
image. `codegen` repeats rexglue codegen -> verify -> add until codegen succeeds, an error cannot be
verified, a round adds nothing new, or the round limit is reached. Every decision and its evidence go
to the report.

`boot` does the same for the runtime's version of the problem: a function no direct branch reaches
(only a pointer, such as a vtable slot without RTTI) passes codegen unregistered, and calling it ends
the run with "[FATAL] Call to invalid or unregistered function at guest address X". Each such X in the
host's logs becomes an entry when the image has code there and the reference lists a function
starting at X; an X inside a reference function is an indirect jump into a function's middle (a jump
table the finder did not recognise, for instance) and is reported, never turned into an entry.

    python -I recomp/tools/resolve_functions.py codegen --rexglue <rexglue> --manifest recomp/ttt2_manifest.toml
        --image recomp/game/default.xex --out recomp/game/ttt2_resolved_functions.toml
        [--reference recomp/ttt2_functions.ttt2web.toml] [--log codegen.log] [--report REPORT.md]
        [--json report.json] [--rounds 8] [--ignore-stamp] [--log-level debug]
    python -I recomp/tools/resolve_functions.py verify --image <plain.xex> --log <codegen.log>
        [--reference ...] [--out ...] [--report ...] [--json ...]
    python -I recomp/tools/resolve_functions.py boot --image <plain.xex> --boot-log <host log> [--boot-log ...]
        [--codegen-log <the codegen log, for the reference's helper fingerprint>] [--reference ...]
        [--out ...] [--report ...] [--json ...]

Exit codes: 0 codegen succeeded (`codegen`) / every unresolved call or called address verified
(`verify`, `boot`); 1 codegen failed for another reason; 3 unresolved calls or called addresses remain
(unverified, no progress or round limit); 2 bad input.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import time

# The XEX2 / PE readers live in find_switch_tables.py next to this file (loaded by path: -I keeps the
# script's directory off sys.path).
_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("find_switch_tables", os.path.join(_HERE, "find_switch_tables.py"))
fst = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(fst)

HELPERS = ("savegprlr_14", "restgprlr_14", "savefpr_14", "restfpr_14",
           "savevmx_14", "restvmx_14", "savevmx_64", "restvmx_64")
BLR, BCTR = 0x4E800020, 0x4E800420


# ---------------------------------------------------------------------------- the image

class Image:
    """The PE image inside a plain XEX2 (or a raw PE image with an explicit base)."""

    def __init__(self, data, base=None):
        if base is None:
            self.pe, self.base = fst.parse_xex2(data)
        else:
            self.pe, self.base = bytes(data), base
        self.sections = fst.pe_sections(self.pe, self.base)
        self.code = [(start, end) for _, start, end, executable in self.sections if executable]
        self.sha1 = hashlib.sha1(data).hexdigest()

    def word(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.pe) or addr & 3:
            return None
        return struct.unpack_from(">I", self.pe, off)[0]

    def in_code(self, addr):
        return any(lo <= addr < hi for lo, hi in self.code)

    def pdata(self):
        """(begin, size) of every .pdata entry (exception directory of the PE optional header)."""
        nt = struct.unpack_from("<I", self.pe, 0x3C)[0]
        opt = nt + 24
        if struct.unpack_from("<H", self.pe, opt)[0] != 0x10B:  # PE32
            return []
        rva, size = struct.unpack_from("<II", self.pe, opt + 96 + 3 * 8)
        entries = []
        for off in range(rva, rva + size - 7, 8):
            if off + 8 > len(self.pe):
                break
            begin, info = struct.unpack_from(">II", self.pe, off)
            length = ((info >> 8) & 0x3FFFFF) * 4
            if begin and length:
                entries.append((begin, length))
        return entries


def decode_branch(addr, w):
    """A direct branch (b/ba/bl/bla or bc*) at `addr` as {mnemonic, target, link, conditional}, else None."""
    op = w >> 26
    if op == 18:
        disp = w & 0x03FFFFFC
        if disp & 0x02000000:
            disp -= 0x04000000
        absolute, link = bool(w & 2), bool(w & 1)
        target = (disp if absolute else addr + disp) & 0xFFFFFFFF
        return {"mnemonic": ("bl" if link else "b") + ("a" if absolute else ""), "target": target,
                "link": link, "conditional": False}
    if op == 16:
        disp = w & 0xFFFC
        if disp & 0x8000:
            disp -= 0x10000
        absolute, link = bool(w & 2), bool(w & 1)
        target = (disp if absolute else addr + disp) & 0xFFFFFFFF
        always = ((w >> 21) & 0x14) == 0x14
        return {"mnemonic": "bc" + ("l" if link else "") + ("a" if absolute else ""), "target": target,
                "link": link, "conditional": not always}
    return None


def falls_through(w):
    """False when execution cannot continue from this word into the next one."""
    if w is None or w in (0, 0xFFFFFFFF, BLR, BCTR):
        return False
    op = w >> 26
    if op == 18 and not (w & 1):
        return False                                    # b / ba
    if op == 16 and not (w & 1) and ((w >> 21) & 0x14) == 0x14:
        return False                                    # branch always
    if op == 3 or (op == 31 and ((w >> 1) & 0x3FF) == 4):
        return False                                    # twi / tw
    return True


# ---------------------------------------------------------------------------- the codegen log

RE_CATEGORY = re.compile(r"(UnresolvedCall|MissingJumpTable|JumpTargetOutOfBounds|DiscontinuousFunction|"
                         r"UnimplementedInsn) \((\d+)\):\s*$")
RE_ENTRY2 = re.compile(r"(0x[0-9A-Fa-f]{8}) from (0x[0-9A-Fa-f]{8}): (.*?)\s*$")
RE_ENTRY1 = re.compile(r"^\s*(?:\[[^\]]*\]\s*)*(0x[0-9A-Fa-f]{8}): (.*?)\s*$")
RE_SEAL = re.compile(r"sealAllReady: (0x[0-9A-Fa-f]{8}) \((\S+)\) cannot seal")
RE_SEAL_EDGE = re.compile(r"^\s+(0x[0-9A-Fa-f]{8}) -> (0x[0-9A-Fa-f]{8})\s*$")
RE_HELPER = re.compile(r"Found __(" + "|".join(HELPERS) + r") at (0x[0-9A-Fa-f]{8})")
RE_TOTAL = re.compile(r"Total: (\d+) errors")
RE_INVALID_CALL = re.compile(r"Call to invalid or unregistered function at guest address (0x[0-9A-Fa-f]{1,8})")


def parse_log(text):
    """Analysis errors, unsealed functions and the helpers ReXGlue found, from a codegen log."""
    errors = {}            # category -> [entry dict]
    seal_owner = {}        # branch site -> (function address, name) of the function that could not seal
    helpers = {}
    current, in_block, total = None, False, None
    owner = None
    for line in text.splitlines():
        m = RE_HELPER.search(line)
        if m:
            helpers[m.group(1)] = int(m.group(2), 16)
        m = RE_SEAL.search(line)
        if m:
            owner = (int(m.group(1), 16), m.group(2))
            continue
        m = RE_SEAL_EDGE.match(line)
        if m and owner:
            seal_owner[int(m.group(1), 16)] = owner
            continue
        owner = None
        if "=== ANALYSIS ERRORS ===" in line:
            in_block, current = True, None
            continue
        if not in_block:
            continue
        m = RE_TOTAL.search(line)
        if m:
            total = int(m.group(1))
            in_block = False
            continue
        m = RE_CATEGORY.search(line)
        if m:
            current = m.group(1)
            errors.setdefault(current, [])
            continue
        if current is None:
            continue
        m = RE_ENTRY2.search(line)
        if m:
            errors[current].append({"address": int(m.group(1), 16), "from": int(m.group(2), 16),
                                    "message": m.group(3)})
            continue
        m = RE_ENTRY1.search(line)
        if m:
            errors[current].append({"address": int(m.group(1), 16), "from": None, "message": m.group(2)})
    unresolved = []
    for e in errors.get("UnresolvedCall", []):
        if e["from"] is None:
            continue
        mnemonic = e["message"].split(" ", 1)[0]
        unresolved.append({"target": e["address"], "site": e["from"], "mnemonic": mnemonic,
                           "message": e["message"], "function": seal_owner.get(e["from"])})
    return {"errors": errors, "unresolved": unresolved, "helpers": helpers, "total": total}


# ---------------------------------------------------------------------------- boundary references

RE_FUNC_LINE = re.compile(r'^\s*"(0x[0-9A-Fa-f]+)"\s*=\s*\{([^}]*)\}', re.M)
RE_SIZE = re.compile(r"\bsize\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
RE_END = re.compile(r"\bend\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
RE_FINGERPRINT = re.compile(r"^#\s+(" + "|".join(HELPERS) + r"):\s*(0x[0-9A-Fa-f]+)", re.M)
RE_IMAGE_SHA1 = re.compile(r"^#\s*image-sha1:\s*([0-9a-f]{40})", re.M)


def parse_functions_toml(text):
    """{address: size or 0} from a ReXGlue [functions] table written one entry per line."""
    out = {}
    for m in RE_FUNC_LINE.finditer(text):
        addr, body = int(m.group(1), 16), m.group(2)
        size = RE_SIZE.search(body)
        end = RE_END.search(body)
        if size:
            out[addr] = int(size.group(1), 0)
        elif end:
            out[addr] = max(int(end.group(1), 0) - addr, 0)
        else:
            out[addr] = 0
    return out


class Reference:
    """Function boundaries from a published table and/or the image's .pdata, for lookups."""

    def __init__(self):
        self.funcs = {}   # start -> (size, source)

    def add(self, start, size, source):
        if size > 0 and start not in self.funcs:
            self.funcs[start] = (size, source)

    def finish(self):
        self.starts = sorted(self.funcs)

    def at(self, addr):
        f = self.funcs.get(addr)
        return (addr, f[0], f[1]) if f else None

    def containing(self, addr):
        import bisect
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            start = self.starts[i]
            size, source = self.funcs[start]
            if start <= addr < start + size:
                return (start, size, source)
        return None

    def neighbours(self, addr):
        import bisect
        i = bisect.bisect_right(self.starts, addr)
        prev = self.starts[i - 1] if i > 0 else None
        nxt = self.starts[i] if i < len(self.starts) else None
        return prev, nxt


def load_reference(path, log_helpers):
    """(functions, fingerprint status) of a reference table; applies only when its helpers match."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    published = {name: int(v, 16) for name, v in RE_FINGERPRINT.findall(text)}
    funcs = parse_functions_toml(text)
    if not published:
        return funcs, {"applies": True, "detail": "the reference carries no helper fingerprint (test data)"}
    missing = [n for n in HELPERS if n in published and n not in log_helpers]
    differ = [n for n in HELPERS if n in published and n in log_helpers and published[n] != log_helpers[n]]
    matched = sum(1 for n in HELPERS if n in published and log_helpers.get(n) == published[n])
    if missing or differ:
        detail = (f"helper fingerprint {matched} of {len(published)} match "
                  f"(differ: {', '.join(differ) or '-'}; not found by ReXGlue: {', '.join(missing) or '-'})")
        return funcs, {"applies": False, "detail": detail}
    return funcs, {"applies": True,
                   "detail": f"helper fingerprint {matched} of {len(published)} match ReXGlue's own helper detection"}


# ---------------------------------------------------------------------------- decisions

def hx(v):
    return f"0x{v:08X}"


def decide(err, image, ref):
    """Verified entry for one UnresolvedCall error, or the reason it cannot be verified."""
    t, s = err["target"], err["site"]
    out = {"target": t, "site": s, "mnemonic": err["mnemonic"], "function": err.get("function")}
    w = image.word(s)
    if w is None or not image.in_code(s):
        return {**out, "decision": "unverified", "evidence": f"the branch site {hx(s)} is not in the image's code"}
    br = decode_branch(s, w)
    if br is None or br["target"] != t:
        return {**out, "decision": "unverified",
                "evidence": f"the instruction at {hx(s)} (0x{w:08X}) is not a direct branch to {hx(t)}: "
                            "the log does not belong to this image"}
    out["instruction"] = f"{br['mnemonic']} {hx(t)}"
    wt = image.word(t)
    if wt is None or not image.in_code(t) or wt in (0, 0xFFFFFFFF):
        return {**out, "decision": "unverified", "evidence": f"the target {hx(t)} is not code"}
    out["target_preceded_by_fallthrough"] = falls_through(image.word(t - 4))
    if ref is None:
        return {**out, "decision": "unverified", "evidence": "no boundary reference applies to this image"}
    start = ref.at(t)
    if start:
        return {**out, "decision": "entry", "entry": t, "size": 0,
                "evidence": f"{out['instruction']} at {hx(s)}; the {start[2]} lists a function starting at "
                            f"{hx(t)} (size 0x{start[1]:X})"}
    f = ref.containing(t)
    if f and f[2] != ".pdata" and f[0] <= s < f[0] + f[1]:
        return {**out, "decision": "function", "entry": f[0], "size": f[1],
                "evidence": f"{out['instruction']} at {hx(s)}; the {f[2]} has one function "
                            f"{hx(f[0])}+0x{f[1]:X} containing both the branch and its target"}
    if f:
        why = (f"the target lies inside {hx(f[0])}+0x{f[1]:X} ({f[2]}) but the branch comes from outside it"
               if f[2] != ".pdata" else
               f"the target lies inside the .pdata function {hx(f[0])}+0x{f[1]:X}")
        return {**out, "decision": "unverified", "evidence": why}
    prev, nxt = ref.neighbours(t)
    return {**out, "decision": "unverified",
            "evidence": "no reference function starts at or contains the target (nearest: "
                        f"{hx(prev) if prev is not None else '-'} below, {hx(nxt) if nxt is not None else '-'} above)"}


def parse_boot_logs(paths):
    """{called address: (log file, line number)} of every invalid-function trap in the host's logs."""
    found = {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = RE_INVALID_CALL.search(line)
                if m:
                    found.setdefault(int(m.group(1), 16), (path, n))
    return found


def decide_runtime(target, origin, image, ref):
    """Verified entry for an address the game called through a pointer, or why it cannot be one."""
    where = f"{os.path.basename(origin[0])} line {origin[1]}"
    out = {"target": target, "site": None, "mnemonic": "indirect call", "function": None,
           "instruction": "indirect call", "origin": where}
    w = image.word(target)
    if w is None or not image.in_code(target) or w in (0, 0xFFFFFFFF):
        return {**out, "decision": "unverified", "evidence": f"the called address {hx(target)} is not code"}
    out["target_preceded_by_fallthrough"] = falls_through(image.word(target - 4))
    if ref is None:
        return {**out, "decision": "unverified", "evidence": "no boundary reference applies to this image"}
    start = ref.at(target)
    if start:
        return {**out, "decision": "entry", "entry": target, "size": 0,
                "evidence": f"the game called {hx(target)} through a pointer ({where}); the {start[2]} lists a "
                            f"function starting there (size 0x{start[1]:X})"}
    f = ref.containing(target)
    if f:
        return {**out, "decision": "unverified",
                "evidence": f"the called address lies inside {hx(f[0])}+0x{f[1]:X} ({f[2]}): an indirect jump into "
                            "the middle of a function (a jump table the finder did not recognise?), not a missing "
                            "function"}
    prev, nxt = ref.neighbours(target)
    return {**out, "decision": "unverified",
            "evidence": "no reference function starts at or contains the called address (nearest: "
                        f"{hx(prev) if prev is not None else '-'} below, {hx(nxt) if nxt is not None else '-'} above)"}


def note_unread_dispatches(decisions, switch_json, image, ref):
    """Adds the switch dispatches the finder could not read to unverified decisions in the same function:
    an indirect jump there is most likely one of that switch's case labels, not a missing function."""
    try:
        with open(switch_json, encoding="utf-8") as f:
            sites = [int(u["bctr"], 16) for u in json.load(f).get("unresolved_sites", [])]
    except (OSError, ValueError, KeyError) as e:
        for d in decisions:
            if d["decision"] == "unverified":
                d["evidence"] += f" (the switch report {switch_json} could not be read: {e})"
        return
    for d in decisions:
        if d["decision"] != "unverified":
            continue
        f = ref.containing(d["target"]) if ref is not None else None
        lo, hi = (f[0], f[0] + f[1]) if f else (d["target"] - 0x2000, d["target"] + 0x2000)
        near = [b for b in sites if lo <= b < hi]
        if near:
            d["unread_dispatches"] = near
            d["evidence"] += ("; the same " + ("function" if f else "code (within 8 KB)") + " holds the switch dispatch"
                              + ("es " if len(near) > 1 else " ") + ", ".join(hx(b) for b in near)
                              + " that the switch-table finder could not read (switch-audit.md lists the code): the "
                              "address is most likely one of its case labels")


def build_reference(image, reference_path, log_helpers):
    """The lookup table (reference file when its fingerprint matches, plus .pdata) and its status."""
    ref = Reference()
    status = {"path": reference_path, "applies": False, "detail": "no reference given"}
    if reference_path:
        funcs, st = load_reference(reference_path, log_helpers)
        status.update(st)
        status["entries"] = len(funcs)
        if st["applies"]:
            for a, size in funcs.items():
                ref.add(a, size, "reference table " + os.path.basename(reference_path))
    pdata = image.pdata()
    for a, size in pdata:
        ref.add(a, size, ".pdata")
    status["pdata_entries"] = len(pdata)
    ref.finish()
    return (ref if (status["applies"] or pdata) else None), status


# ---------------------------------------------------------------------------- the output file

HEADER = """# Function boundaries the pinned ReXGlue analysis misses on this image, added by
# recomp/tools/resolve_functions.py. Each entry was verified before it was added: the branch the
# codegen log names was decoded from the image, and a boundary reference (TTT2 Web's published
# table when its helper fingerprint matches, and the image's .pdata) agrees; the comment after each
# entry gives the evidence. Addresses and sizes only. Valid only for the image with this SHA-1:
# image-sha1: {sha1}
[functions]
"""


def read_out(path):
    """(entries {addr: (size, comment)}, image sha1) of an existing output file."""
    if not path or not os.path.exists(path):
        return {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = RE_IMAGE_SHA1.search(text)
    entries = {}
    for line in text.splitlines():
        mm = RE_FUNC_LINE.match(line)
        if not mm:
            continue
        size = parse_functions_toml(line).get(int(mm.group(1), 16), 0)
        rest = line[mm.end():].strip()
        comment = rest[1:].strip() if rest.startswith("#") else ""
        entries[int(mm.group(1), 16)] = (size, comment)
    return entries, (m.group(1) if m else None)


def overlaps(entries):
    """(a, b) pairs where an entry lies inside a sized entry: ReXGlue rejects such a config."""
    bad = []
    sized = sorted((a, s) for a, (s, _) in entries.items() if s)
    for a, s in sized:
        for b in entries:
            if b != a and a < b < a + s:
                bad.append((a, b))
    return bad


def write_out(path, entries, sha1):
    lines = [HEADER.format(sha1=sha1).rstrip("\n")]
    for a in sorted(entries):
        size, comment = entries[a]
        body = f"{{ size = 0x{size:X} }}" if size else "{}"
        lines.append(f'"0x{a:08X}" = {body}' + (f"  # {comment}" if comment else ""))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def apply_decisions(entries, decisions):
    """Adds verified decisions to `entries`; returns the addresses added and the conflicts found."""
    added, conflicts = [], []
    for d in decisions:
        if d["decision"] not in ("entry", "function"):
            continue
        a, size = d["entry"], d["size"]
        if d.get("site") is None:
            comment = f"{d['decision']}: called through a pointer at run time ({d.get('origin', 'boot log')})"
        else:
            comment = (f"{d['decision']}: {d['instruction']} at {hx(d['site'])}"
                       + (f" (in {d['function'][1]})" if d.get("function") else ""))
        if a in entries:
            if entries[a][0] != size:
                conflicts.append(f"{hx(a)} is already present with size 0x{entries[a][0]:X}, now 0x{size:X}")
                d["decision"], d["evidence"] = "unverified", d["evidence"] + "; conflicts with an earlier entry"
            continue
        trial = dict(entries)
        trial[a] = (size, comment)
        bad = overlaps(trial)
        if bad:
            conflicts.append(f"{hx(a)}: would overlap " + ", ".join(f"{hx(x)}/{hx(y)}" for x, y in bad))
            d["decision"], d["evidence"] = "unverified", d["evidence"] + "; would overlap another entry"
            continue
        entries[a] = (size, comment)
        added.append(a)
    return added, conflicts


# ---------------------------------------------------------------------------- reporting

def markdown(state):
    lines = ["# Function-boundary resolution (recomp/tools/resolve_functions.py)", ""]
    lines.append(f"* Image: `{state['image']}` (SHA-1 {state['image_sha1']})")
    ref = state["reference"]
    if ref.get("path"):
        lines.append(f"* Reference: `{ref['path']}`: {'applies' if ref['applies'] else 'does NOT apply'}"
                     f" ({ref['detail']}); {ref.get('entries', 0)} entries; image .pdata: {ref.get('pdata_entries', 0)} entries")
    else:
        lines.append(f"* Reference: none; image .pdata: {ref.get('pdata_entries', 0)} entries")
    lines.append(f"* Output: `{state['out']}` ({len(state['entries'])} entries)")
    lines.append(f"* Result: **{state['result']}**")
    if state.get("discarded"):
        lines.append(f"* {state['discarded']}")
    for r in state["rounds"]:
        title = (f"## Boot logs: {r['unresolved']} calls to unregistered functions" if r.get("boot")
                 else f"## Round {r['round']}: rexglue exit {r['rc']}, {r['unresolved']} unresolved calls")
        lines += ["", title + (f", other errors: {r['other']}" if r.get("other") else "")]
        if not r["decisions"]:
            continue
        lines += ["", "| target | branch | in function | decision | evidence |", "|---|---|---|---|---|"]
        for d in r["decisions"]:
            fn = d["function"][1] if d.get("function") else "-"
            dec = d["decision"] + (f" {hx(d['entry'])}" + (f"+0x{d['size']:X}" if d.get("size") else "")
                                   if d["decision"] in ("entry", "function") else "")
            branch = (f"{d.get('instruction', d['mnemonic'])} at {hx(d['site'])}" if d.get("site") is not None
                      else f"indirect call ({d.get('origin', 'boot log')})")
            lines.append(f"| {hx(d['target'])} | {branch} | {fn} | {dec} | {d['evidence']} |")
        if r.get("added"):
            lines.append("")
            lines.append(f"Added {len(r['added'])} entries: " + ", ".join(hx(a) for a in r["added"]))
        for c in r.get("conflicts", []):
            lines.append(f"* conflict: {c}")
    return "\n".join(lines) + "\n"


def write_reports(state, report, json_path):
    if report:
        with open(report, "w", encoding="utf-8", newline="\n") as f:
            f.write(markdown(state))
    if json_path:
        serial = dict(state)
        serial["entries"] = {hx(a): {"size": s, "comment": c} for a, (s, c) in state["entries"].items()}
        with open(json_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(serial, f, indent=2, default=lambda o: hx(o) if isinstance(o, int) else str(o))


# ---------------------------------------------------------------------------- one round

def resolve_round(image, log_text, reference_path, entries):
    parsed = parse_log(log_text)
    ref, status = build_reference(image, reference_path, parsed["helpers"])
    decisions = [decide(e, image, ref) for e in parsed["unresolved"]]
    added, conflicts = apply_decisions(entries, decisions)
    other = {c: len(v) for c, v in parsed["errors"].items() if c != "UnresolvedCall" and v}
    return parsed, status, decisions, added, conflicts, other


def load_image(path, base):
    with open(path, "rb") as f:
        data = f.read()
    return Image(data, int(base, 16) if base else None)


def start_entries(out, image, state):
    entries, sha1 = read_out(out)
    if entries and sha1 != image.sha1:
        state["discarded"] = (f"{len(entries)} entries of {out} were for another image (SHA-1 {sha1}) and "
                              "were discarded")
        entries = {}
    return entries


def cmd_verify(args):
    image = load_image(args.image, args.base)
    with open(args.log, encoding="utf-8", errors="replace") as f:
        log_text = f.read()
    state = {"image": args.image, "image_sha1": image.sha1, "out": args.out or "-", "rounds": []}
    entries = start_entries(args.out, image, state)
    parsed, status, decisions, added, conflicts, other = resolve_round(image, log_text, args.reference, entries)
    state["reference"] = status
    state["rounds"].append({"round": 1, "rc": None, "unresolved": len(parsed["unresolved"]), "other": other,
                            "decisions": decisions, "added": added, "conflicts": conflicts})
    unverified = [d for d in decisions if d["decision"] == "unverified"]
    if args.out:
        write_out(args.out, entries, image.sha1)
    state["entries"] = entries
    if not parsed["unresolved"]:
        state["result"] = "no UnresolvedCall errors in the log"
        rc = 0
    elif unverified:
        state["result"] = f"{len(unverified)} of {len(decisions)} unresolved calls could not be verified"
        rc = 3
    else:
        state["result"] = f"all {len(decisions)} unresolved calls verified ({len(added)} new entries)"
        rc = 0
    write_reports(state, args.report, args.json)
    print(f"resolve_functions: {state['result']}")
    for d in decisions:
        print(f"  {hx(d['target'])} from {hx(d['site'])}: {d['decision']}: {d['evidence']}")
    return rc


def cmd_boot(args):
    image = load_image(args.image, args.base)
    targets = parse_boot_logs(args.boot_log)
    helpers = {}
    if args.codegen_log:
        with open(args.codegen_log, encoding="utf-8", errors="replace") as f:
            helpers = parse_log(f.read())["helpers"]
    state = {"image": args.image, "image_sha1": image.sha1, "out": args.out or "-", "rounds": []}
    entries = start_entries(args.out, image, state)
    ref, status = build_reference(image, args.reference, helpers)
    state["reference"] = status
    decisions = [decide_runtime(t, origin, image, ref) for t, origin in sorted(targets.items())]
    if getattr(args, "switch_json", None):
        note_unread_dispatches(decisions, args.switch_json, image, ref)
    before = set(entries)
    added, conflicts = apply_decisions(entries, decisions)
    repeated = [d for d in decisions if d["decision"] == "entry" and d["entry"] in before]
    state["rounds"].append({"round": 1, "rc": None, "boot": True, "unresolved": len(targets), "decisions": decisions,
                            "added": added, "conflicts": conflicts})
    if args.out:
        write_out(args.out, entries, image.sha1)
    state["entries"] = entries
    state["added"] = added
    unverified = [d for d in decisions if d["decision"] == "unverified"]
    if not targets:
        state["result"] = "no calls to unregistered functions in the boot logs"
        rc = 0
    elif unverified:
        state["result"] = (f"{len(unverified)} of {len(targets)} called addresses could not be verified "
                           f"({len(added)} verified entries added)")
        rc = 3
    else:
        state["result"] = (f"all {len(targets)} called addresses verified ({len(added)} new entries"
                           + (f"; {len(repeated)} already present: the host still lacked them, rebuild it" if repeated else "")
                           + ")")
        rc = 0
    write_reports(state, args.report, args.json)
    print(f"resolve_functions: {state['result']}")
    for d in decisions:
        print(f"  {hx(d['target'])}: {d['decision']}: {d['evidence']}")
    return rc


def run_rexglue(args, round_no, log_path):
    manifest = os.path.abspath(args.manifest)
    cmd = [os.path.abspath(args.rexglue), "--log-level", args.log_level, "codegen"]
    if args.ignore_stamp or round_no > 1:
        cmd.append("--ignore-stamp")
    cmd.append(os.path.basename(manifest))
    started = time.time()
    with open(log_path, "w", encoding="utf-8", errors="replace", newline="\n") as f:
        rc = subprocess.run(cmd, cwd=os.path.dirname(manifest), stdout=f, stderr=subprocess.STDOUT).returncode
    return rc, time.time() - started


def cmd_codegen(args):
    if not os.path.isfile(args.rexglue):
        print(f"resolve_functions: rexglue not found: {args.rexglue}", file=sys.stderr)
        return 2
    image = load_image(args.image, args.base)
    state = {"image": args.image, "image_sha1": image.sha1, "out": args.out, "rounds": [],
             "reference": {"path": args.reference, "applies": False, "detail": "not evaluated (codegen succeeded)"}}
    entries = start_entries(args.out, image, state)
    write_out(args.out, entries, image.sha1)        # the manifest includes it: it must exist
    log = args.log or os.path.join(os.path.dirname(os.path.abspath(args.manifest)), "codegen.log")
    stem, ext = os.path.splitext(log)
    rc = 1
    for round_no in range(1, args.rounds + 1):
        rc, seconds = run_rexglue(args, round_no, log)
        with open(log, encoding="utf-8", errors="replace") as f:
            log_text = f.read()
        print(f"resolve_functions: round {round_no}: rexglue exit {rc} after {seconds:.1f} s")
        if rc == 0:
            state["rounds"].append({"round": round_no, "rc": 0, "unresolved": 0, "decisions": []})
            state["result"] = (f"codegen succeeded in round {round_no}"
                               + (f" with {len(entries)} resolved entries" if entries else ""))
            break
        parsed, status, decisions, added, conflicts, other = resolve_round(image, log_text, args.reference, entries)
        state["reference"] = status
        state["rounds"].append({"round": round_no, "rc": rc, "unresolved": len(parsed["unresolved"]),
                                "other": other, "decisions": decisions, "added": added, "conflicts": conflicts,
                                "log": f"{stem}.round{round_no}{ext}"})
        for d in decisions:
            print(f"  {hx(d['target'])} from {hx(d['site'])}: {d['decision']}: {d['evidence']}")
        # keep this round's log next to the final one
        with open(f"{stem}.round{round_no}{ext}", "w", encoding="utf-8", newline="\n") as f:
            f.write(log_text)
        if not parsed["unresolved"]:
            state["result"] = (f"codegen failed in round {round_no} for a reason other than unresolved calls"
                               + (f" ({', '.join(f'{k} {v}' for k, v in other.items())})" if other else "")
                               + f"; see {log}")
            rc = 1
            break
        unverified = [d for d in decisions if d["decision"] == "unverified"]
        write_out(args.out, entries, image.sha1)
        if unverified:
            state["result"] = (f"stopped in round {round_no}: {len(unverified)} unresolved calls could not be "
                               f"verified ({len(added)} verified entries were added)")
            rc = 3
            break
        if not added:
            state["result"] = f"stopped in round {round_no}: no new entries (the same errors came back)"
            rc = 3
            break
        print(f"resolve_functions: round {round_no}: added {len(added)} verified entries; rerunning codegen")
    else:
        state["result"] = f"stopped: unresolved calls remain after {args.rounds} rounds"
        rc = 3
    state["entries"] = entries
    write_reports(state, args.report, args.json)
    print(f"resolve_functions: {state['result']}")
    return 0 if rc == 0 else rc


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--image", required=True, help="the plain XEX codegen reads (recomp/game/default.xex)")
    common.add_argument("--base", help="load address when --image is a raw PE image (hex)")
    common.add_argument("--reference", help="boundary reference ([functions] TOML with a helper fingerprint)")
    common.add_argument("--report", help="Markdown report to write")
    common.add_argument("--json", help="JSON report to write")
    v = sub.add_parser("verify", parents=[common], help="verify the unresolved calls of one codegen log")
    v.add_argument("--log", required=True)
    v.add_argument("--out", help="[functions] file to add verified entries to")
    c = sub.add_parser("codegen", parents=[common], help="run rexglue codegen, resolving unresolved calls")
    c.add_argument("--rexglue", required=True)
    c.add_argument("--manifest", required=True)
    c.add_argument("--out", required=True, help="[functions] file the manifest includes")
    c.add_argument("--log", help="codegen log (default: codegen.log next to the manifest)")
    c.add_argument("--rounds", type=int, default=8)
    c.add_argument("--ignore-stamp", action="store_true", help="pass --ignore-stamp to the first run too")
    c.add_argument("--log-level", default="debug")
    bt = sub.add_parser("boot", parents=[common], help="verify the unregistered functions a host run called")
    bt.add_argument("--boot-log", action="append", required=True, help="host log file (repeatable)")
    bt.add_argument("--codegen-log", help="codegen log of the build, for the reference's helper fingerprint")
    bt.add_argument("--out", help="[functions] file to add verified entries to")
    bt.add_argument("--switch-json", help="find_switch_tables.py --json report: names the switch dispatch it could "
                                          "not read when a called address lies in the same function")
    args = ap.parse_args(argv[1:])
    try:
        if args.command == "boot":
            return cmd_boot(args)
        return cmd_verify(args) if args.command == "verify" else cmd_codegen(args)
    except (OSError, ValueError, struct.error) as e:
        print(f"resolve_functions: error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
