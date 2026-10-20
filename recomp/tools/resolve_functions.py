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

`codegen --indirect` finds those functions before the game calls them, so a boot does not have to trap
on each one (one rebuild per trap). After a successful codegen it reads the functions ReXGlue
registered (the generated *_register.cpp) and considers every reference function missing there. One is
registered ahead of time only with evidence from the image itself: a 4-aligned word in one of its data
sections (not code, .pdata or .reloc) holds its address, the rule TTT2 Web's table was derived with for
vtables and callbacks; and the code checks pass: the image has code there, the instruction before it
cannot fall through into it, it lies inside no .pdata function (exception-handler labels) and is no
switch case label (--switch-json, the finder's report), its reference extent is plain code ending in a
branch or return, and ReXGlue registered nothing inside that extent. A reference function no data word
points at is never registered this way. The verified ones go in as one batch of entries (marked "ahead
of time") and codegen runs again. If it then fails in a way the error resolution cannot fix, the batch
entries the errors point into are taken out, listed in the output file so later runs skip them, and
codegen reruns; if that does not help, the whole batch is rolled back to the entries that compiled
before, and codegen reruns on those. After the final successful round every entry of the output file
must be registered in the generated code; the report names any that is not. `--drop-indirect` removes
the ahead-of-time entries and keeps every other one.

    python -I recomp/tools/resolve_functions.py codegen --rexglue <rexglue> --manifest recomp/ttt2_manifest.toml
        --image recomp/game/default.xex --out recomp/game/ttt2_resolved_functions.toml
        [--reference recomp/ttt2_functions.ttt2web.toml] [--log codegen.log] [--report REPORT.md]
        [--json report.json] [--rounds 8] [--ignore-stamp] [--log-level debug]
        [--indirect --switch-json switch_tables.json | --drop-indirect]
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
import array
import bisect
import glob
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


def plural(n, word):
    return f"{n} {word}" + ("" if n == 1 else "s")


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


# ---------------------------------------------------------------------------- ahead of time: functions a pointer reaches

POINTER_SKIP = (".pdata", ".reloc")       # exception directory and relocations: no function pointers
_WORD = next(t for t in "IL" if array.array(t).itemsize == 4)
RE_OUT_DIR = re.compile(r'^\s*out_directory_path\s*=\s*"([^"]*)"', re.M)
RE_SET_FUNCTION = re.compile(r"SetFunction\(0x([0-9A-Fa-f]+),")
INDIRECT_REASONS = {     # reason key -> heading in the report, in the order the checks run
    "excluded": "taken out in an earlier run (codegen failed with it)",
    "no-pointer": "no pointer to it in the image's data: no evidence, not registered",
    "not-code": "no code at the address",
    "extent": "its reference extent is not plain code ending in a branch or return",
    "fallthrough": "the instruction before it falls through into it",
    "pdata": "inside a .pdata function (exception-handler label or other code of that function)",
    "switch-label": "a switch case label",
    "overlap": "ReXGlue registered a function inside its reference extent",
    "conflict": "conflicts with an entry of the output file",
}


def manifest_out_dir(manifest):
    """The directory a ReXGlue manifest's codegen writes to (its out_directory_path, relative to it)."""
    with open(manifest, encoding="utf-8", errors="replace") as f:
        m = RE_OUT_DIR.search(f.read())
    rel = m.group(1) if m else "generated/default"
    return os.path.join(os.path.dirname(os.path.abspath(manifest)), *rel.replace("\\", "/").split("/"))


def registered_functions(gen_dir):
    """Every function address the generated code registers (its *_register.cpp), or None without one."""
    files = sorted(glob.glob(os.path.join(gen_dir, "*_register.cpp")))
    if not files:
        return None
    reg = set()
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as f:
            reg.update(int(m.group(1), 16) for m in RE_SET_FUNCTION.finditer(f.read()))
    return reg


def switch_labels(path):
    """{case label: its bctr} of every table in a find_switch_tables.py --json report."""
    with open(path, encoding="utf-8") as f:
        report = json.load(f)
    labels = {}
    for t in report.get("tables", []):
        for label in t.get("labels", []):
            labels.setdefault(int(label, 16), int(t["bctr"], 16))
    return labels


def pointer_sites(image, targets):
    """{target: [(site, section)]}: the 4-aligned words of the image's data sections (every section that is
    not executable, .pdata or .reloc) that hold one of `targets`. Code is not searched: an instruction word
    can equal a code address (0x82D6FC78 decodes as lwz r22,-904(r22))."""
    targets = set(targets)
    found = {}
    if not targets:
        return found
    for name, lo, hi, executable in image.sections:
        if executable or name in POINTER_SKIP:
            continue
        start = (max(lo, image.base) + 3) & ~3
        end = min(hi, image.base + len(image.pe)) & ~3
        if end <= start:
            continue
        words = array.array(_WORD, image.pe[start - image.base:end - image.base])
        if sys.byteorder == "little":
            words.byteswap()
        hits = targets.intersection(words)
        if not hits:
            continue
        for i, w in enumerate(words):
            if w in hits:
                found.setdefault(w, []).append((start + 4 * i, name))
    return found


def _indirect_check(image, t, size, ptrs, reg, pdata, pstarts, labels, excluded):
    """(reason key, evidence) of the first check reference function `t` fails, or (None, None)."""
    if t in excluded:
        return "excluded", f"taken out in an earlier run: {excluded[t]}"
    if not ptrs:
        return "no-pointer", "no 4-aligned word in the image's data sections holds its address"
    w = image.word(t)
    if w is None or not image.in_code(t) or w in (0, 0xFFFFFFFF):
        return "not-code", f"the image has no code at {hx(t)}"
    end = t + max(size, 4)
    section = next(((lo, hi) for lo, hi in image.code if lo <= t < hi), None)
    if section is None or end > section[1]:
        return "extent", f"its reference extent {hx(t)}+0x{size:X} leaves the code section"
    for a in range(t, end, 4):
        if image.word(a) in (0, None):
            return "extent", f"its reference extent {hx(t)}+0x{size:X} holds a zero word at {hx(a)}"
    if falls_through(image.word(end - 4)):
        return "extent", (f"its reference extent {hx(t)}+0x{size:X} ends at {hx(end - 4)} with an instruction that "
                          "falls through (into the next function)")
    prev = image.word(t - 4) if image.in_code(t - 4) else None
    if falls_through(prev):
        return "fallthrough", (f"the instruction before it ({hx(t - 4)}: 0x{prev:08X}) falls through into it: a "
                               "point inside a function, not an entry")
    k = bisect.bisect_right(pstarts, t) - 1
    if k >= 0 and pdata[k][0] < t < pdata[k][0] + pdata[k][1]:
        return "pdata", f"it lies inside the .pdata function {hx(pdata[k][0])}+0x{pdata[k][1]:X}"
    if t in labels:
        return "switch-label", f"it is a case label of the switch at {hx(labels[t])}"
    i = bisect.bisect_right(reg, t)
    if i < len(reg) and reg[i] < end:
        return "overlap", f"ReXGlue registered {hx(reg[i])} inside its reference extent: the boundaries disagree"
    return None, None


def decide_indirect(image, ref, registered, labels, entries, excluded):
    """Decisions for every reference function ReXGlue did not register (and no entry names yet): an entry
    ahead of time when a data word points at it and the code checks pass, otherwise the reason."""
    reg = sorted(registered)
    pdata = sorted(image.pdata())
    pstarts = [b for b, _ in pdata]
    candidates = [s for s in ref.starts if s not in registered and s not in entries]
    sites = pointer_sites(image, candidates)
    decisions = []
    for t in candidates:
        size, source = ref.funcs[t]
        out = {"target": t, "site": None, "mnemonic": "pointer", "function": None, "kind": "pointer",
               "instruction": "pointer in data", "reference_size": size}
        ptrs = sites.get(t, [])
        reason, why = _indirect_check(image, t, size, ptrs, reg, pdata, pstarts, labels, excluded)
        if reason:
            decisions.append({**out, "decision": "unverified", "reason": reason, "evidence": why})
            continue
        site, section = ptrs[0]
        origin = f"pointer at {hx(site)} ({section})" + (f" and {len(ptrs) - 1} more" if len(ptrs) > 1 else "")
        decisions.append({**out, "decision": "entry", "entry": t, "size": 0, "origin": origin, "reason": "registered",
                          "pointers": [p for p, _ in ptrs[:16]],
                          "evidence": f"{origin}; the {source} lists a function starting at {hx(t)} "
                                      f"(size 0x{size:X}); the code checks pass"})
    return decisions


def error_addresses(parsed):
    """Every address the analysis errors of a codegen log name (targets and branch sites)."""
    out = set()
    for errs in parsed["errors"].values():
        for e in errs:
            out.add(e["address"])
            if e.get("from") is not None:
                out.add(e["from"])
    return out


# ---------------------------------------------------------------------------- the output file

HEADER = """# Function boundaries the pinned ReXGlue analysis misses on this image, added by
# recomp/tools/resolve_functions.py. Each entry was verified before it was added: a boundary
# reference (TTT2 Web's published table when its helper fingerprint matches, and the image's .pdata)
# agrees with evidence from this image: a branch a codegen error names, decoded from the image; an
# address the game called at run time (boot log); or, for the entries marked "ahead of time", a
# pointer in the image's data plus the code checks. The comment after each entry gives the evidence.
# Addresses and sizes only. Valid only for the image with this SHA-1:
# image-sha1: {sha1}
[functions]
"""
EXCLUDED_HEADER = """
# Not entries: functions a pointer reaches that were registered ahead of time once and taken out
# again because codegen failed with them. `codegen --indirect` skips them; the boot step still adds
# one that the game calls.
"""
AOT_MARK = "ahead of time"
AOT_PREFIX = f"entry: {AOT_MARK}"
RE_EXCLUDED = re.compile(r"^#\s*excluded (0x[0-9A-Fa-f]{8}): (.*?)\s*$", re.M)


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


def read_exclusions(path):
    """{address: reason} of the functions an earlier run took out of the ahead-of-time batch."""
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8", errors="replace") as f:
        return {int(m.group(1), 16): m.group(2) for m in RE_EXCLUDED.finditer(f.read())}


def write_out(path, entries, sha1, excluded=None):
    lines = [HEADER.format(sha1=sha1).rstrip("\n")]
    for a in sorted(entries):
        size, comment = entries[a]
        body = f"{{ size = 0x{size:X} }}" if size else "{}"
        lines.append(f'"0x{a:08X}" = {body}' + (f"  # {comment}" if comment else ""))
    if excluded:
        lines.append(EXCLUDED_HEADER.rstrip("\n"))
        for a in sorted(excluded):
            lines.append(f"# excluded {hx(a)}: " + " ".join(str(excluded[a]).split()))
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
        if d.get("kind") == "pointer":
            comment = f"{AOT_PREFIX}, {d['origin']}"
        elif d.get("site") is None:
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
    aot_count = sum(1 for _, c in state["entries"].values() if c.startswith(AOT_PREFIX))
    lines.append(f"* Output: `{state['out']}` ({len(state['entries'])} entries"
                 + (f", {aot_count} of them registered ahead of time" if aot_count else "") + ")")
    lines.append(f"* Result: **{state['result']}**")
    if state.get("discarded"):
        lines.append(f"* {state['discarded']}")
    if state.get("dropped") is not None:
        lines.append(f"* --drop-indirect removed {len(state['dropped'])} entries registered ahead of time")
    if state.get("unregistered_entries"):
        missing = state["unregistered_entries"]
        lines.append(f"* PROBLEM: {len(missing)} entries of the output file are not registered in the generated code "
                     "(ReXGlue ignored them): " + ", ".join(hx(a) for a in missing[:40]) + (" ..." if len(missing) > 40 else ""))
    elif "unregistered_entries" in state:
        lines.append("* Every entry of the output file is registered in the generated code")
    for r in state["rounds"]:
        title = (f"## Boot logs: {r['unresolved']} calls to unregistered functions" if r.get("boot")
                 else f"## Round {r['round']}: rexglue exit {r['rc']}, {r['unresolved']} unresolved calls"
                 + (f"; then {plural(len(r['indirect_added']), 'function')} registered ahead of time"
                    if r.get("indirect_added") else ""))
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
    lines += indirect_markdown(state.get("indirect"))
    return "\n".join(lines) + "\n"


def indirect_markdown(aot):
    """The report section of the ahead-of-time pass (counts by reason, with examples)."""
    if not aot or not aot.get("enabled"):
        return []
    lines = ["", "## Ahead of time: functions only a pointer in the image's data reaches (--indirect)", ""]
    lines.append(f"* Outcome: **{aot.get('outcome', aot.get('status', '?'))}**")
    if aot.get("detail"):
        lines.append(f"* {aot['detail']}")
    decisions = aot.get("decisions") or []
    if "candidates" in aot:
        lines.append(f"* {aot['candidates']} reference functions have no registered function at their address "
                     f"({aot['registered']} functions registered in the generated code)")
    added = aot.get("added") or []
    if added:
        lines.append(f"* Registered ahead of time: {len(added)}: " + ", ".join(hx(a) for a in added[:40])
                     + (" ... (all of them are in the output file, marked \"ahead of time\")" if len(added) > 40 else ""))
    for key, title in (("taken_out", "Taken out again after codegen failed with them"),
                       ("rolled_back", "Rolled back with the whole batch")):
        if aot.get(key):
            lines.append(f"* {title}: {len(aot[key])}: " + ", ".join(hx(a) for a in aot[key][:40])
                         + (" ..." if len(aot[key]) > 40 else ""))
    rejected = {}
    for d in decisions:
        if d["decision"] != "entry":
            rejected.setdefault(d.get("reason", "?"), []).append(d)
    if rejected:
        lines += ["", "| not registered ahead of time | count | examples (address: evidence) |", "|---|---|---|"]
        for key in list(INDIRECT_REASONS) + sorted(k for k in rejected if k not in INDIRECT_REASONS):
            ds = rejected.get(key)
            if not ds:
                continue
            examples = "; ".join(f"{hx(d['target'])}: {d['evidence']}" for d in ds[:3])
            lines.append(f"| {INDIRECT_REASONS.get(key, key)} | {len(ds)} | {examples} |")
    for c in aot.get("conflicts") or []:
        lines.append(f"* conflict: {c}")
    return lines


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


def start_exclusions(out, image):
    """The exclusions an earlier run recorded in `out`, when that file belongs to this image."""
    _, sha1 = read_out(out)
    return read_exclusions(out) if sha1 == image.sha1 else {}


def cmd_verify(args):
    image = load_image(args.image, args.base)
    with open(args.log, encoding="utf-8", errors="replace") as f:
        log_text = f.read()
    state = {"image": args.image, "image_sha1": image.sha1, "out": args.out or "-", "rounds": []}
    entries = start_entries(args.out, image, state)
    excluded = start_exclusions(args.out, image)
    parsed, status, decisions, added, conflicts, other = resolve_round(image, log_text, args.reference, entries)
    state["reference"] = status
    state["rounds"].append({"round": 1, "rc": None, "unresolved": len(parsed["unresolved"]), "other": other,
                            "decisions": decisions, "added": added, "conflicts": conflicts})
    unverified = [d for d in decisions if d["decision"] == "unverified"]
    if args.out:
        write_out(args.out, entries, image.sha1, excluded)
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
    excluded = start_exclusions(args.out, image)
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
        write_out(args.out, entries, image.sha1, excluded)
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
    # One line each for scripts (the owner pipeline's boot loop reads these, not the JSON).
    print(f"resolve_functions: boot summary: {len(targets)} called, {len(added)} added, {len(repeated)} already present, "
          f"{len(unverified)} unverified")
    if added:
        print("resolve_functions: added " + " ".join(hx(a) for a in added))
    if repeated:
        print("resolve_functions: already present " + " ".join(hx(d["entry"]) for d in repeated))
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


def indirect_pass(args, image, log_text, entries, excluded):
    """The ahead-of-time pass after a successful codegen round: (decisions, added, info, reference)."""
    gen = manifest_out_dir(args.manifest)
    registered = registered_functions(gen)
    if registered is None:
        return [], [], {"status": "skipped", "detail": f"the generated code in {gen} has no *_register.cpp"}, None
    try:
        labels = switch_labels(args.switch_json)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        return [], [], {"status": "skipped", "detail": f"the switch report {args.switch_json} could not be read: {e}"}, None
    ref, status = build_reference(image, args.reference, parse_log(log_text)["helpers"])
    info = {"reference": status, "registered": len(registered)}
    if ref is None:
        return [], [], {**info, "status": "skipped", "detail": "no boundary reference applies to this image"}, None
    decisions = decide_indirect(image, ref, registered, labels, entries, excluded)
    added, conflicts = apply_decisions(entries, decisions)
    for d in decisions:
        if d["decision"] == "unverified" and d.get("reason") == "registered":
            d["reason"] = "conflict"
    return decisions, added, {**info, "status": "evaluated", "candidates": len(decisions), "conflicts": conflicts}, ref


def cmd_codegen(args):
    if not os.path.isfile(args.rexglue):
        print(f"resolve_functions: rexglue not found: {args.rexglue}", file=sys.stderr)
        return 2
    image = load_image(args.image, args.base)
    state = {"image": args.image, "image_sha1": image.sha1, "out": args.out, "rounds": [],
             "reference": {"path": args.reference, "applies": False, "detail": "not evaluated (codegen succeeded)"}}
    entries = start_entries(args.out, image, state)
    excluded = start_exclusions(args.out, image)
    if getattr(args, "drop_indirect", False):
        dropped = sorted(a for a, (_, c) in entries.items() if c.startswith(AOT_PREFIX))
        for a in dropped:
            del entries[a]
        excluded = {}
        state["dropped"] = dropped
        print(f"resolve_functions: --drop-indirect: removed {len(dropped)} entries registered ahead of time "
              "and the list of exclusions")
    write_out(args.out, entries, image.sha1, excluded)        # the manifest includes it: it must exist
    log = args.log or os.path.join(os.path.dirname(os.path.abspath(args.manifest)), "codegen.log")
    stem, ext = os.path.splitext(log)
    aot = {"enabled": bool(getattr(args, "indirect", False)), "status": "pending"}
    state["indirect"] = aot
    batch = None            # the entries registered ahead of time that codegen has not accepted yet
    limit, round_no, rc = args.rounds, 0, 1

    def roll_back(why):
        # Back to the entries that compiled before the batch (everything added since depends on it); the batch's
        # functions are remembered as exclusions, so later runs do not try them again.
        entries.clear()
        entries.update(batch["snapshot"])
        for a in batch["added"]:
            excluded[a] = f"rolled back with the batch registered ahead of time: {why}"
        aot.update(status="rolled back", rolled_back=list(batch["added"]), added=[],
                   outcome=f"rolled back: {why}; every entry from before the batch is kept")
        write_out(args.out, entries, image.sha1, excluded)
        n = len(batch["added"])
        print(f"resolve_functions: {why}: rolled back the {n} {'entry' if n == 1 else 'entries'} registered ahead of "
              "time; rerunning codegen on the entries that compiled before")

    while True:
        round_no += 1
        if round_no > limit:
            if batch is None:
                state["result"] = f"stopped: unresolved calls remain after {args.rounds} rounds"
                rc = 3
                break
            roll_back(f"unresolved calls still came back after {limit} rounds")
            batch, limit = None, round_no
        rc, seconds = run_rexglue(args, round_no, log)
        with open(log, encoding="utf-8", errors="replace") as f:
            log_text = f.read()
        print(f"resolve_functions: round {round_no}: rexglue exit {rc} after {seconds:.1f} s")
        if rc == 0:
            state["rounds"].append({"round": round_no, "rc": 0, "unresolved": 0, "decisions": []})
            if batch is not None:
                aot.update(status="accepted", added=list(batch["added"]),
                           outcome=f"{plural(len(batch['added']), 'function')} registered ahead of time; codegen "
                                   f"accepted {'it' if len(batch['added']) == 1 else 'them'} in round {round_no}")
                batch = None
            elif aot["enabled"] and aot["status"] == "pending":
                snapshot = dict(entries)
                decisions, added, info, ref = indirect_pass(args, image, log_text, entries, excluded)
                aot.update(info)
                aot.update(decisions=decisions, added=list(added))
                if info.get("reference"):
                    state["reference"] = info["reference"]
                if added:
                    batch = {"added": list(added), "snapshot": snapshot, "retried": False,
                             "extents": {a: max(ref.funcs[a][0], 4) for a in added}}
                    aot["status"] = "batch"
                    state["rounds"][-1]["indirect_added"] = list(added)
                    write_out(args.out, entries, image.sha1, excluded)
                    print(f"resolve_functions: round {round_no}: registered {plural(len(added), 'function')} that a "
                          "pointer in the image's data reaches, ahead of time; rerunning codegen")
                    continue
                aot.update(status="done", outcome=("nothing new to register ahead of time" if info["status"] == "evaluated"
                                                   else f"skipped: {info.get('detail', '')}"))
            n_aot = sum(1 for _, c in entries.values() if c.startswith(AOT_PREFIX))
            state["result"] = (f"codegen succeeded in round {round_no}"
                               + (f" with {len(entries)} resolved entries" if entries else "")
                               + (f" ({n_aot} of them ahead of time)" if n_aot else ""))
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
        unverified = [d for d in decisions if d["decision"] == "unverified"]
        if not parsed["unresolved"]:
            failure = ("other", f"codegen failed in round {round_no} for a reason other than unresolved calls"
                                + (f" ({', '.join(f'{k} {v}' for k, v in other.items())})" if other else "")
                                + f"; see {log}")
        elif unverified:
            failure = ("unverified", f"stopped in round {round_no}: {len(unverified)} unresolved calls could not be "
                                     f"verified ({len(added)} verified entries were added)")
        elif not added:
            failure = ("no-progress", f"stopped in round {round_no}: no new entries (the same errors came back)")
        else:
            failure = None
        write_out(args.out, entries, image.sha1, excluded)
        if failure is None:
            print(f"resolve_functions: round {round_no}: added {len(added)} verified entries; rerunning codegen")
            continue
        if batch is not None:
            # Codegen failed with the entries registered ahead of time in a way the error resolution cannot fix.
            # The errors are reported, never suppressed: first the batch entries they point into are taken out,
            # then, if codegen still fails, the whole batch.
            summary = ", ".join(f"{k} {len(v)}" for k, v in parsed["errors"].items() if v) or f"rexglue exit {rc}"
            why = f"codegen failed in round {round_no} ({summary})"
            errs = error_addresses(parsed)
            hit = [a for a in batch["added"] if any(a <= e < a + batch["extents"][a] for e in errs)]
            if hit and len(hit) < len(batch["added"]) and not batch["retried"] and round_no < limit:
                batch["retried"] = True
                for a in hit:
                    entries.pop(a, None)
                    excluded[a] = f"{why} with it registered ahead of time"
                batch["added"] = [a for a in batch["added"] if a not in hit]
                aot.setdefault("taken_out", []).extend(hit)
                aot["added"] = list(batch["added"])
                write_out(args.out, entries, image.sha1, excluded)
                print(f"resolve_functions: {why}: the errors point into {len(hit)} of the functions registered ahead "
                      "of time; taking them out and rerunning codegen")
                continue
            roll_back(why)
            batch, limit = None, max(limit, round_no + 1)
            continue
        state["result"] = failure[1]
        rc = 1 if failure[0] == "other" else 3
        break
    if rc == 0:
        reg = registered_functions(manifest_out_dir(args.manifest))
        if reg is not None:
            state["unregistered_entries"] = sorted(a for a in entries if a not in reg)
            if state["unregistered_entries"]:
                print(f"resolve_functions: PROBLEM: {len(state['unregistered_entries'])} entries are not registered in "
                      "the generated code: " + " ".join(hx(a) for a in state["unregistered_entries"][:20]))
    if aot["enabled"] and "outcome" not in aot:
        aot["outcome"] = "not run: codegen did not succeed"
    state["entries"] = entries
    write_reports(state, args.report, args.json)
    print(f"resolve_functions: {state['result']}")
    if aot["enabled"]:
        print(f"resolve_functions: ahead of time: {aot['outcome']}")
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
    c.add_argument("--indirect", action="store_true",
                   help="after a successful codegen, register ahead of time the reference functions ReXGlue did not "
                        "register whose address a word in the image's data holds (needs --switch-json); rerun codegen")
    c.add_argument("--switch-json", help="find_switch_tables.py --json report of the image (its case labels are "
                                         "never registered as functions)")
    c.add_argument("--drop-indirect", action="store_true",
                   help="first remove the entries registered ahead of time and the list of exclusions (every other "
                        "entry stays)")
    bt = sub.add_parser("boot", parents=[common], help="verify the unregistered functions a host run called")
    bt.add_argument("--boot-log", action="append", required=True, help="host log file (repeatable)")
    bt.add_argument("--codegen-log", help="codegen log of the build, for the reference's helper fingerprint")
    bt.add_argument("--out", help="[functions] file to add verified entries to")
    bt.add_argument("--switch-json", help="find_switch_tables.py --json report: names the switch dispatch it could "
                                          "not read when a called address lies in the same function")
    args = ap.parse_args(argv[1:])
    if args.command == "codegen" and args.indirect and not args.switch_json:
        ap.error("codegen --indirect needs --switch-json (the finder's report of the image)")
    try:
        if args.command == "boot":
            return cmd_boot(args)
        return cmd_verify(args) if args.command == "verify" else cmd_codegen(args)
    except (OSError, ValueError, struct.error) as e:
        print(f"resolve_functions: error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
