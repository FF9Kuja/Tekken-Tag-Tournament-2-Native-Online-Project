#!/usr/bin/env python3
"""Finds switch jump tables in an Xbox 360 executable image and writes ReXGlue [[switch_tables]] entries.

ReXGlue 0.10.0 recognises a jump table by pattern-matching the dispatch sequence backwards from
each `bctr`. Measured on recomp/tests/ppc/switch_ttt2.s, it finds every absolute (lwzx) table but
misses byte- and halfword-offset tables whenever the `add` accumulates into the offset register.
Tekken Tag Tournament 2's compiler also schedules the sequence in an order XenonAnalyse does not
expect (TTT2 Web, CC0). This tool does not pattern-match: it finds the bounds check and
symbolically executes the instructions from there to the `bctr`, so any ordering of
lis / addi / ori / mr / rlwinm / lwzx / lbzx / lhzx / add is handled:

    absolute : target = load32(table + index * 4)
    offset   : target = base + (load8|16(table + index * n) << shift)

Approach after TTT2 Web's tools/find_switch_tables.py (CC0); written for this project's pipeline.

    python3 -I recomp/tools/find_switch_tables.py <image.xex | image.bin --base 0x82000000>
                                                 [--toml switch_tables.toml] [--json report.json]
                                                 [--window 24] [--max-entries 4096]

Input: an unencrypted, uncompressed XEX2 (what ttt2-xexpatch produces, or a test image), or a
raw PE image with --base. Output: TOML `[[switch_tables]]` entries (address = the bctr,
register = the index register the generated switch tests, labels = targets in index order) and a
JSON summary with the unresolved dispatches for diagnosis. Addresses only, never game bytes.
Exit code 0 on success (even with zero tables), 2 on input errors.
"""
import argparse
import hashlib
import json
import struct
import sys

BCTR = 0x4E800420
BLR = 0x4E800020
WINDOW = 24


# ---------------------------------------------------------------------------- input formats

def parse_xex2(data):
    """Returns (image bytes, load address) of an unencrypted, uncompressed XEX2, or raises."""
    if data[:4] != b"XEX2":
        raise ValueError("not an XEX2 file")
    header_size, security_offset, count = struct.unpack_from(">I", data, 8)[0], struct.unpack_from(">I", data, 0x10)[0], struct.unpack_from(">I", data, 0x14)[0]
    if count > 1024 or 0x18 + count * 8 > header_size or header_size > len(data):
        raise ValueError("damaged XEX2 header")
    load_address = struct.unpack_from(">I", data, security_offset + 0x110)[0]
    image_size = struct.unpack_from(">I", data, security_offset + 0x4)[0]
    for i in range(count):
        key, value = struct.unpack_from(">II", data, 0x18 + i * 8)
        if key == 0x000003FF:  # FILE_FORMAT_INFO
            encryption, compression = struct.unpack_from(">HH", data, value + 4)
            if encryption != 0 or compression != 0:
                raise ValueError("the image is encrypted or compressed (encryption %d, compression %d): "
                                 "run ttt2-xexpatch first, it writes a plain image" % (encryption, compression))
    image = data[header_size:header_size + image_size]
    if len(image) < image_size:
        raise ValueError("the file is shorter than its declared image size")
    return bytes(image), load_address


def pe_sections(image, base):
    """(name, start, end, executable) for every PE section, plus the .pdata range if present."""
    if image[:2] != b"MZ":
        raise ValueError("the image does not start with an MZ header")
    nt = struct.unpack_from("<I", image, 0x3C)[0]
    if image[nt:nt + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    nsections = struct.unpack_from("<H", image, nt + 6)[0]
    optsize = struct.unpack_from("<H", image, nt + 20)[0]
    sections = []
    for i in range(nsections):
        off = nt + 24 + optsize + i * 40
        name = image[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, rva = struct.unpack_from("<II", image, off + 8)
        chars = struct.unpack_from("<I", image, off + 36)[0]
        executable = bool(chars & 0x20000000) or bool(chars & 0x20)
        sections.append((name, base + rva, base + rva + vsize, executable))
    return sections


# ---------------------------------------------------------------------------- decoding helpers

def fields(i):
    return {
        "op": i >> 26, "rd": (i >> 21) & 31, "ra": (i >> 16) & 31, "rb": (i >> 11) & 31,
        "simm": struct.unpack(">h", struct.pack(">H", i & 0xFFFF))[0], "uimm": i & 0xFFFF,
        "xo": (i >> 1) & 0x3FF, "sh": (i >> 11) & 31, "mb": (i >> 6) & 31, "me": (i >> 1) & 31,
    }


def is_mtctr(i):
    return (i >> 26) == 31 and ((i >> 1) & 0x3FF) == 467 and (((i >> 16) & 31) | (((i >> 11) & 31) << 5)) == 9


def is_branch(i):
    return (i >> 26) in (16, 18, 19)


def dest_reg(i):
    """Integer register written by a non-branch instruction, or None."""
    f = fields(i)
    op = f["op"]
    if op in (7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 58):
        return f["rd"]  # mulli subfic addic addic. addi addis lwz lwzu lbz lbzu lhz lhzu lha lhau ld
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
        return f["ra"]  # rlwimi rlwinm rlwnm ori oris xori xoris andi. andis.
    if op == 31:
        xo = f["xo"]
        if xo in (24, 26, 27, 28, 58, 60, 122, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824, 826, 922, 954, 986):
            return f["ra"]  # shifts, logical ops, extsb/extsh/extsw, cntlzw
        if xo in (0, 32, 467, 144, 150, 151, 183, 215, 247, 407, 439, 54, 86, 278, 470, 982, 598):
            return None  # compares, mtspr, stwcx., stores, cache ops, sync
        return f["rd"]
    return None


# ---------------------------------------------------------------------------- analysis

class Finder:
    def __init__(self, image, base, sections, window=WINDOW, max_entries=4096):
        self.image, self.base, self.sections = image, base, sections
        self.window, self.max_entries = window, max_entries
        self.code = [(s, e) for _, s, e, x in sections if x]
        pdata = [(s, e) for n, s, e, _ in sections if n == ".pdata"]
        self.functions = []
        if pdata:
            s, e = pdata[0]
            for off in range(s, e, 8):
                begin, info = self.word(off), self.word(off + 4)
                if begin:
                    self.functions.append((begin, begin + ((info >> 8) & 0x3FFFFF) * 4))
            self.functions.sort()

    def word(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.image):
            return None
        return struct.unpack_from(">I", self.image, off)[0]

    def in_code(self, addr):
        return any(s <= addr < e for s, e in self.code)

    def enclosing_function(self, addr):
        for s, e in self.functions:
            if s <= addr < e:
                return s, e
        return None

    # -- bounds check -----------------------------------------------------------------------

    def find_bounds(self, bctr):
        """Returns (start, index_reg, count, aliases, recompute) or None.

        The dispatch block starts after a conditional branch (bc or bclr) whose CR field a
        `cmplwi crN, rI, max` set; the compiler may schedule unrelated instructions between the
        compare and the branch, copy the index with `mr` first, or recompute a biased index
        after the branch with the same addi it used before the compare."""
        for k in range(2, self.window):
            a = bctr - 4 * k
            i = self.word(a)
            if i is None:
                return None
            is_bc = (i >> 26) == 16
            is_bclr_cond = (i >> 26) == 19 and ((i >> 1) & 0x3FF) == 16 and i != BLR
            if is_bc or is_bclr_cond:
                crf = (i >> 18) & 7
                for j in range(1, 33):
                    c = a - 4 * j
                    w = self.word(c)
                    if w is None:
                        return None
                    f = fields(w)
                    if f["op"] in (10, 11) and (f["rd"] & 1) == 0 and (f["rd"] >> 2) == crf:  # cmplwi / cmpwi
                        if any(dest_reg(self.word(x)) == f["ra"] for x in range(c + 4, a, 4)):
                            return None  # the index changes between compare and branch
                        idx, count = f["ra"], (f["uimm"] if f["op"] == 10 else f["simm"]) + 1
                        if count < 2 or count > self.max_entries:
                            return None
                        recompute = None
                        aliases = set()
                        for m in range(1, 7):
                            g = self.word(c - 4 * m)
                            if g is None:
                                break
                            gf = fields(g)
                            if dest_reg(g) == idx and gf["op"] in (14, 15) and gf["ra"] and gf["ra"] != idx:
                                if not any(dest_reg(self.word(x)) == gf["ra"] for x in range(c - 4 * m + 4, bctr, 4)):
                                    recompute = g
                                break
                        for m in range(1, 7):
                            g = self.word(c - 4 * m)
                            if g is None:
                                break
                            gf = fields(g)
                            if gf["op"] == 31 and gf["xo"] == 444 and gf["rd"] == gf["rb"]:
                                if gf["rd"] == idx:
                                    aliases.add(gf["ra"])  # mr rX, rIdx: the copy holds the index
                                elif gf["ra"] == idx:
                                    aliases.add(gf["rd"])  # mr rIdx, rX: the compare ran on a copy
                        return a + 4, idx, count, aliases, recompute
                    if is_branch(w):
                        return None
                    if f["op"] in (10, 11) and (f["rd"] >> 2) == crf:
                        return None  # another compare feeds the branch
                    if f["op"] == 31 and f["xo"] in (0, 32) and (f["rd"] >> 2) == crf:
                        return None
                return None
            if is_branch(i) or i == BCTR:
                return None
        return None

    def find_byte_switch(self, bctr):
        """An unbounded byte switch: `lbz rI` feeds the dispatch, 256 entries, no bounds check."""
        for k in range(2, self.window):
            a = bctr - 4 * k
            i = self.word(a)
            if i is None or is_branch(i) or i == BCTR:
                return None
            if (i >> 26) == 34:
                rd = (i >> 21) & 31
                if any(dest_reg(self.word(x)) == rd for x in range(a + 4, bctr, 4)):
                    return None
                return a + 4, rd, 256, set(), None
        return None

    # -- symbolic execution -------------------------------------------------------------------

    def emulate(self, bctr, start, idx_reg, aliases, count, recompute):
        regs = {r: ("idx", 0) for r in aliases | {idx_reg}}
        for a in range(start, bctr - 4, 4):
            i = self.word(a)
            if i is None or is_branch(i):
                return None
            f = fields(i)
            op = f["op"]
            val = None
            if i == recompute:
                val = ("idx", 0)
            elif op == 15 and f["ra"] == 0:  # lis
                val = ("const", (f["uimm"] << 16) & 0xFFFFFFFF)
            elif op == 14:  # addi / li
                if f["ra"] == 0:
                    val = ("const", f["simm"] & 0xFFFFFFFF)
                elif regs.get(f["ra"], ("?",))[0] == "const":
                    val = ("const", (regs[f["ra"]][1] + f["simm"]) & 0xFFFFFFFF)
            elif op == 24 and f["uimm"] == 0:  # ori rA,rS,0 = copy
                val = regs.get(f["rd"])
            elif op == 24 and regs.get(f["rd"], ("?",))[0] == "const":  # ori
                val = ("const", regs[f["rd"]][1] | f["uimm"])
            elif op == 31 and f["xo"] == 444 and f["rd"] == f["rb"]:  # mr
                val = regs.get(f["rd"])
            elif op == 21:  # rlwinm as slwi
                src = regs.get(f["rd"])
                if src and f["mb"] == 0 and f["me"] == 31 - f["sh"]:
                    if src[0] == "idx":
                        val = ("idx", src[1] + f["sh"])
                    elif src[0] == "load":
                        val = ("load", src[1], src[2], src[3], src[4] + f["sh"])
            elif op == 31 and f["xo"] in (23, 87, 279):  # lwzx / lbzx / lhzx
                size = {23: 4, 87: 1, 279: 2}[f["xo"]]
                ra, rb = regs.get(f["ra"]), regs.get(f["rb"])
                for tbl, ix in ((ra, rb), (rb, ra)):
                    if tbl and ix and tbl[0] == "const" and ix[0] == "idx" and (1 << ix[1]) == size:
                        val = ("load", size, tbl[1], None, 0)
            elif op == 31 and f["xo"] == 266:  # add
                ra, rb = regs.get(f["ra"]), regs.get(f["rb"])
                for c, l in ((ra, rb), (rb, ra)):
                    if c and l and c[0] == "const" and l[0] == "load" and l[3] is None:
                        val = ("load", l[1], l[2], c[1], l[4])
            d = dest_reg(i)
            if d is not None:
                if d == idx_reg and val is None:
                    return None  # the index register is clobbered before the bctr
                if val is None:
                    regs.pop(d, None)
                else:
                    regs[d] = val
        mt = self.word(bctr - 4)
        if mt is None or not is_mtctr(mt):
            return None
        target = regs.get(fields(mt)["rd"])
        if not target or target[0] != "load":
            return None
        _, size, table, add_base, shift = target
        if (size == 4) != (add_base is None):
            return None
        # The generated `switch` tests one register for the case numbers 0..n-1, so that register
        # must still hold the UNSCALED index at the bctr. Prefer the compared register, then any
        # alias; an index scaled in place (rlwinm rI, rI, 2) leaves none and is reported instead.
        unscaled = [r for r in [idx_reg] + sorted(aliases) if regs.get(r) == ("idx", 0)]
        if not unscaled:
            unscaled = [r for r, v in sorted(regs.items()) if v == ("idx", 0)]
        scale = 0
        if unscaled:
            idx_reg = unscaled[0]
        else:
            # Only a scaled copy survives (rlwinm rI, rI, k): switch on it and stretch the label
            # list by 2^k, repeating each label into the unreachable in-between case numbers.
            scaled = [(v[1], r) for r, v in sorted(regs.items()) if v[0] == "idx" and v[1] > 0]
            if not scaled:
                return "scaled-in-place"
            scale, idx_reg = min(scaled)
            if count << scale > self.max_entries:
                return "scaled-in-place"
        labels = []
        for n in range(count):
            p = table + n * size
            if size == 4:
                w = self.word(p)
                if w is None:
                    return None
                labels.append(w)
            else:
                off = p - self.base
                if off < 0 or off + size > len(self.image):
                    return None
                raw = int.from_bytes(self.image[off:off + size], "big")
                labels.append((add_base + (raw << shift)) & 0xFFFFFFFF)
        if scale:
            labels = [label for label in labels for _ in range(1 << scale)]
        return idx_reg, size, table, labels, scale

    def analyse(self, bctr):
        bounds = self.find_bounds(bctr)
        kind = "bounded"
        if bounds is None:
            bounds = self.find_byte_switch(bctr)
            kind = "byte-switch"
        if bounds is None:
            return None
        start, idx_reg, count, aliases, recompute = bounds
        res = self.emulate(bctr, start, idx_reg, aliases, count, recompute)
        if res is None:
            return {"ok": False, "bctr": bctr, "kind": kind, "reason": "dispatch sequence not recognised"}
        if res == "scaled-in-place":
            return {"ok": False, "bctr": bctr, "kind": kind,
                    "reason": "no register holds the unscaled index at the bctr (scaled in place)"}
        idx_reg, size, table, labels, scale = res
        if kind == "byte-switch":
            # The real table is usually shorter than 256: keep labels up to the first one that
            # leaves the enclosing function (or is not an instruction address).
            fn = self.enclosing_function(bctr) or (bctr - 0x8000, bctr + 0x8000)
            kept = []
            for label in labels:
                if label & 3 or not (fn[0] <= label < fn[1]):
                    break
                kept.append(label)
            labels = kept
        if len(labels) < 2 or any(l & 3 or not self.in_code(l) for l in labels):
            return {"ok": False, "bctr": bctr, "kind": kind, "reason": "labels outside code"}
        return {"ok": True, "bctr": bctr, "kind": kind, "register": idx_reg, "entry_size": size,
                "table": table, "labels": labels, "scale": scale}

    def run(self):
        found, unresolved, indirect = [], [], 0
        for s, e in self.code:
            for a in range(s, e, 4):
                if self.word(a) != BCTR:
                    continue
                res = self.analyse(a)
                if res is None:
                    indirect += 1  # no bounds check and no byte load: a computed/vtable call
                elif res["ok"]:
                    found.append(res)
                else:
                    unresolved.append(res)
        return found, unresolved, indirect


# ---------------------------------------------------------------------------- output

def write_toml(path, found, source_name, digest):
    lines = [f"# Generated by recomp/tools/find_switch_tables.py from {source_name} (SHA-1 {digest}).",
             f"# {len(found)} switch tables: address = bctr, register = index register, labels in index order.",
             "# Include from the codegen config: includes = [\"<this file>\"]. Addresses only; no game bytes.", ""]
    for t in found:
        lines += ["[[switch_tables]]", f"address = 0x{t['bctr']:08X}", f"register = {t['register']}",
                  "labels = [" + ", ".join(f"0x{l:08X}" for l in t["labels"]) + "]",
                  f"# {t['kind']}, {t['entry_size']}-byte entries at 0x{t['table']:08X}"
                  + (f"; the index register is scaled by {1 << t['scale']} at the bctr, labels stretched to match"
                     if t["scale"] else ""), ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--base", help="load address for a raw PE image (hex); omit for an XEX2 file")
    ap.add_argument("--toml")
    ap.add_argument("--json")
    ap.add_argument("--window", type=int, default=WINDOW)
    ap.add_argument("--max-entries", type=int, default=4096)
    args = ap.parse_args(argv[1:])
    try:
        with open(args.image, "rb") as f:
            data = f.read()
        digest = hashlib.sha1(data).hexdigest()
        if args.base:
            image, base = data, int(args.base, 16)
        else:
            image, base = parse_xex2(data)
        sections = pe_sections(image, base)
    except (OSError, ValueError, struct.error) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finder = Finder(image, base, sections, args.window, args.max_entries)
    found, unresolved, indirect = finder.run()
    kinds = {}
    for t in found:
        kinds[t["entry_size"]] = kinds.get(t["entry_size"], 0) + 1
    print(f"{args.image}: {len(found)} switch tables ({', '.join(f'{v} with {k}-byte entries' for k, v in sorted(kinds.items())) or 'none'}), "
          f"{len(unresolved)} dispatches with a bounds check but unrecognised sequence, {indirect} other bctr sites")
    for u in unresolved[:20]:
        print(f"  unresolved bctr 0x{u['bctr']:08X} ({u['kind']}: {u.get('reason', '?')})")
    if args.toml:
        write_toml(args.toml, found, args.image.replace("\\", "/").split("/")[-1], digest)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"image": args.image, "sha1": digest, "base": f"0x{base:08X}", "found": len(found),
                       "unresolved": len(unresolved), "other_bctr": indirect,
                       "tables": [{**t, "bctr": f"0x{t['bctr']:08X}", "table": f"0x{t['table']:08X}",
                                   "labels": [f"0x{l:08X}" for l in t["labels"]]} for t in found],
                       "unresolved_sites": [{"bctr": f"0x{u['bctr']:08X}", "kind": u["kind"], "reason": u.get("reason", "")}
                                            for u in unresolved]}, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
