#!/usr/bin/env python3
"""Finds switch jump tables in an Xbox 360 executable image and writes ReXGlue [[switch_tables]] entries.

ReXGlue 0.10.0 recognises a jump table by pattern-matching the dispatch sequence backwards from each
`bctr`. Measured on recomp/tests/ppc/switch_ttt2.s, it misses byte- and halfword-offset tables whenever
the `add` accumulates into the offset register, and for absolute tables it can switch on the register
that holds the loaded target (docs/RESEARCH.md 8a). Tekken Tag Tournament 2's compiler also schedules
the sequence in an order XenonAnalyse does not expect (TTT2 Web, CC0). This tool does not
pattern-match. It symbolically executes the straight-line code that ends at each `bctr` (up to
--window instructions, back to the last instruction that does not fall through), so any ordering and
register allocation of the dispatch arithmetic is handled, and it only uses exact facts:

    absolute : target = load32(table + index * 4)
    offset   : target = base + (load8|16(table + index * n) << shift)     (zero- or sign-extended)

Every register value is a constant, (symbol << k) + constant, a table entry or an unknown symbol, so
copies (mr), recomputations of a biased index, a bias folded into the table address, the table
address set before the bounds check, `add` + D-form loads, 64-bit shifts (sldi, clrldi) and masks
that keep a bounded index are all followed. The index range comes from a bounds check (cmplwi or cmpwi
and a branch away when the index is out of range: `bgt` gives max + 1 entries, `bge` gives max, a
conditional return works the same), from a byte load (256 entries, the table is then cut at the end of
the enclosing function) or from a mask (clrlwi, andi.). Instructions the model does not know make
every register unknown; calls make the volatile ones unknown.

Other paths into the dispatch are checked too. Every branch target from the same function, every table
label and every function start inside the dispatch code is a join point. Facts from before a join point
that precedes the bounds check are dropped (the analysis restarts there), and a join point after the
bounds check must read the same table entry for the same switch register value, or the site is reported
instead of guessed.

Approach after TTT2 Web's tools/find_switch_tables.py (CC0); written for this project's pipeline.

    python3 -I recomp/tools/find_switch_tables.py <image.xex | image.bin --base 0x82000000>
                                                 [--toml switch_tables.toml] [--json report.json]
                                                 [--window 64] [--max-entries 4096]

Input: an unencrypted, uncompressed XEX2 (what ttt2-xexpatch or `ttt2-xexinfo --write-plain` produces,
or a test image), or a raw PE image with --base. Output: TOML `[[switch_tables]]` entries (address =
the bctr, register = the index register the generated switch tests, labels = targets in index order)
and a JSON summary. For each dispatch that is not resolved the JSON names the reason and lists the
disassembled dispatch code (`sequence`), a short local diagnostic for the owner's report: it is never
committed (the pipeline writes it under recomp/game, which git ignores). Exit code 0 on success (even
with zero tables), 2 on input errors.
"""
import argparse
import hashlib
import json
import struct
import sys

BCTR = 0x4E800420
BLR = 0x4E800020
WINDOW = 64
M32 = 0xFFFFFFFF
VOLATILE = frozenset([0] + list(range(3, 13)))


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
                raise ValueError("the image is encrypted or compressed (encryption %d, compression %d): prepare the plain "
                                 "image first with `ttt2-xexinfo <file.xex> --write-plain <plain.xex>` (the owner pipeline "
                                 "does this in step 5c; ttt2-xexpatch's output is plain already) and pass that file"
                                 % (encryption, compression))
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


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def is_mtctr(i):
    return (i >> 26) == 31 and ((i >> 1) & 0x3FF) == 467 and (((i >> 16) & 31) | (((i >> 11) & 31) << 5)) == 9


def is_branch(i):
    return (i >> 26) in (16, 18, 19)


def branch_target(addr, w):
    """Target of a direct branch (b/bl/bc and their absolute forms), or None."""
    op = w >> 26
    if op == 18:
        d = w & 0x03FFFFFC
        if d & 0x02000000:
            d -= 0x04000000
    elif op == 16:
        d = w & 0xFFFC
        if d & 0x8000:
            d -= 0x10000
    else:
        return None
    return (d if w & 2 else addr + d) & M32


def falls_through(w):
    """False when execution never continues at the next instruction (b, blr, bctr, rfi...)."""
    op = w >> 26
    if op == 18:
        return bool(w & 1)
    if op == 16:
        return ((w >> 21) & 0x14) != 0x14 or bool(w & 1)
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo in (16, 528):
            return ((w >> 21) & 0x14) != 0x14 or bool(w & 1)
        return xo not in (18, 50, 82, 274)
    return w != 0


# The general-purpose registers an instruction writes (the low 32 bits are what this tool models).
# None means the instruction is not modelled: the analysis then forgets every register.
_XO_ARITH = {266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 491, 459, 233, 489, 457}
_XO_RD = ({20, 21, 23, 84, 87, 279, 341, 343, 532, 534, 790, 19, 83, 339, 371, 75, 11, 73, 9, 310}
          | _XO_ARITH | {x + 512 for x in _XO_ARITH})
_XO_RD_RA = {53, 55, 119, 311, 373, 375}
_XO_RA = {24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824, 826, 827,
          922, 954, 986, 181, 183, 247, 439, 567, 631, 695, 759}
_XO_NONE = {0, 32, 4, 68, 467, 144, 146, 178, 150, 214, 149, 151, 215, 407, 660, 662, 918, 663, 727,
            983, 535, 599, 54, 86, 246, 278, 470, 982, 1014, 598, 854, 6, 38, 7, 39, 71, 103, 359, 135,
            167, 199, 231, 487, 519, 551, 775, 807, 647, 679, 903, 935, 438}


def gpr_writes(w):
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15, 32, 34, 40, 42):
        return {rd}
    if op in (33, 35, 41, 43):
        return {rd, ra}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 37, 39, 45, 49, 51, 53, 55):
        return {ra}
    if op == 46:
        return set(range(rd, 32))
    if op == 58:
        return {rd, ra} if (w & 3) == 1 else {rd}
    if op == 62:
        return {ra} if (w & 3) == 1 else set()
    if op in (2, 3, 4, 5, 6, 10, 11, 36, 38, 44, 47, 48, 50, 52, 54, 59, 63):
        return set()
    if op in (16, 18):
        return set(VOLATILE) if w & 1 else set()
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo in (16, 528):
            return set(VOLATILE) if w & 1 else set()
        if xo in (0, 33, 129, 150, 193, 225, 257, 289, 417, 449):
            return set()
        return None
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in _XO_RD:
            return {rd}
        if xo in _XO_RD_RA:
            return {rd, ra}
        if xo in _XO_RA:
            return {ra}
        if xo in _XO_NONE:
            return set()
    return None


def _writes_or_all(w):
    r = gpr_writes(w or 0)
    return range(32) if r is None else r


# ---------------------------------------------------------------------------- disassembly (diagnostics)

_D_LOADS = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
            40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
            48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu"}
_X31 = {23: "lwzx", 55: "lwzux", 87: "lbzx", 119: "lbzux", 279: "lhzx", 311: "lhzux", 343: "lhax", 375: "lhaux",
        341: "lwax", 21: "ldx", 151: "stwx", 215: "stbx", 407: "sthx", 149: "stdx", 266: "add", 778: "addo",
        10: "addc", 138: "adde", 40: "subf", 552: "subfo", 8: "subfc", 136: "subfe", 104: "neg", 235: "mullw",
        491: "divw", 459: "divwu", 75: "mulhw", 11: "mulhwu", 233: "mulld", 28: "and", 60: "andc", 124: "nor",
        284: "eqv", 316: "xor", 412: "orc", 444: "or", 476: "nand", 24: "slw", 536: "srw", 792: "sraw",
        824: "srawi", 27: "sld", 539: "srd", 794: "srad", 922: "extsh", 954: "extsb", 986: "extsw",
        26: "cntlzw", 58: "cntlzd", 19: "mfcr", 144: "mtcrf", 278: "dcbt", 246: "dcbtst", 1014: "dcbz",
        86: "dcbf", 54: "dcbst", 598: "sync", 854: "eieio", 103: "lvx", 231: "stvx", 20: "lwarx", 150: "stwcx."}
_COND = {(1, 1): "gt", (1, 0): "le", (0, 1): "lt", (0, 0): "ge", (2, 1): "eq", (2, 0): "ne", (3, 1): "so", (3, 0): "ns"}


def disasm(addr, w):
    """A short, readable rendering of the common integer instructions (diagnostics only)."""
    if w is None:
        return "(unmapped)"
    op = w >> 26
    rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    simm, uimm = sext16(w & 0xFFFF), w & 0xFFFF
    if w == 0x60000000:
        return "nop"
    if op == 14:
        return f"li r{rd},{simm}" if ra == 0 else f"addi r{rd},r{ra},{simm}"
    if op == 15:
        return f"lis r{rd},{simm}" if ra == 0 else f"addis r{rd},r{ra},{simm}"
    if op in (12, 13):
        return f"addic{'.' if op == 13 else ''} r{rd},r{ra},{simm}"
    if op == 7:
        return f"mulli r{rd},r{ra},{simm}"
    if op == 8:
        return f"subfic r{rd},r{ra},{simm}"
    if op in (10, 11):
        return f"{'cmplwi' if op == 10 else 'cmpwi'} cr{rd >> 2},r{ra},{uimm if op == 10 else simm}" + (" (L=1)" if rd & 1 else "")
    if op in (24, 25, 26, 27, 28, 29):
        name = {24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis."}[op]
        return f"{name} r{ra},r{rd},0x{uimm:X}"
    if op in (20, 21, 23):
        name = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op]
        shv = f"r{rb}" if op == 23 else str(rb)
        return f"{name}{'.' if w & 1 else ''} r{ra},r{rd},{shv},{(w >> 6) & 31},{(w >> 1) & 31}"
    if op == 30:
        sh = rb | (((w >> 1) & 1) << 5)
        m = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        name = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}.get((w >> 2) & 7, "rld?")
        return f"{name} r{ra},r{rd},{sh},{m}"
    if op in _D_LOADS:
        return f"{_D_LOADS[op]} r{rd},{simm}(r{ra})"
    if op == 58:
        return f"{['ld', 'ldu', 'lwa', 'ld?'][w & 3]} r{rd},{sext16(w & 0xFFFC)}(r{ra})"
    if op == 62:
        return f"{['std', 'stdu', 'std?', 'std?'][w & 3]} r{rd},{sext16(w & 0xFFFC)}(r{ra})"
    if op == 16:
        t, bo, bi, lk = branch_target(addr, w), rd, ra, "l" if w & 1 else ""
        if bo & 0x14 == 0x14:
            return f"b{lk} 0x{t:08X}"
        if not bo & 4:
            return f"bc{lk} {bo},{bi},0x{t:08X}"
        crf = f"cr{bi >> 2}," if bi >> 2 else ""
        return f"b{_COND[(bi & 3, (bo >> 3) & 1)]}{lk} {crf}0x{t:08X}"
    if op == 18:
        return f"b{'l' if w & 1 else ''}{'a' if w & 2 else ''} 0x{branch_target(addr, w):08X}"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo in (16, 528):
            bo, bi, reg, lk = rd, ra, "lr" if xo == 16 else "ctr", "l" if w & 1 else ""
            if bo & 0x14 == 0x14:
                return f"b{reg}{lk}"
            if not bo & 4:
                return f"bc{reg}{lk} {bo},{bi}"
            crf = f" cr{bi >> 2}" if bi >> 2 else ""
            return f"b{_COND[(bi & 3, (bo >> 3) & 1)]}{reg}{lk}{crf}"
        return {150: "isync", 0: "mcrf"}.get(xo, f"op19 xo={xo}")
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 467 or xo == 339:
            spr = ra | (rb << 5)
            name = {8: "lr", 9: "ctr", 1: "xer"}.get(spr, f"spr{spr}")
            return f"mt{name} r{rd}" if xo == 467 else f"mf{name} r{rd}"
        if xo in (0, 32):
            return f"{'cmpw' if xo == 0 else 'cmplw'} cr{rd >> 2},r{ra},r{rb}" + (" (L=1)" if rd & 1 else "")
        if xo == 444 and rd == rb:
            return f"mr r{ra},r{rd}"
        name = _X31.get(xo)
        if name is None:
            return f"op31 xo={xo} r{rd},r{ra},r{rb}"
        if xo in (24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 922, 954, 986):
            return f"{name}{'.' if w & 1 else ''} r{ra},r{rd}" + ("" if xo in (26, 58, 922, 954, 986) else f",r{rb}")
        if xo == 824:
            return f"srawi r{ra},r{rd},{rb}"
        return f"{name}{'.' if w & 1 else ''} r{rd},r{ra},r{rb}"
    return f"op{op}"


# ---------------------------------------------------------------------------- symbolic execution
#
# Register values (low 32 bits):
#   ("c", v)                                   the constant v
#   ("l", s, k, c)                             (S << k) + c, where S is an unknown value named s
#   ("t", size, signed, s, k, A, shift, base)  the table entry read at A + (S << k), (1 << k) == size,
#                                              extended, shifted left by `shift`, plus `base` (None: no add)
#   ("n", t)                                   the 32-bit word read at t (a "t" value): a second-level table
# Symbols 0..31 are the registers' values where the analysed code starts; every result the model cannot
# express gets a fresh symbol. A bound (c, count, ...) on symbol s means 0 <= S + c < count on the path
# being followed (unsigned).

def _mask32(mb, me):
    m = 0
    for b in range(32):
        if (mb <= b <= me) if mb <= me else (b >= mb or b <= me):
            m |= 1 << (31 - b)
    return m


def _add_const(v, c):
    if v is None:
        return None
    if v[0] == "c":
        return ("c", (v[1] + c) & M32)
    if v[0] == "l":
        return ("l", v[1], v[2], (v[3] + c) & M32)
    if v[0] == "t":
        return v[:7] + ((((v[7] or 0) + c) & M32),)
    return None


def _add(va, vb):
    if va is None or vb is None:
        return None
    if va[0] == "c":
        return _add_const(vb, va[1])
    if vb[0] == "c":
        return _add_const(va, vb[1])
    return None


def _shift(v, sh):
    """(v << sh) mod 2^32, or None when the model cannot express it."""
    if v[0] == "c":
        return ("c", (v[1] << sh) & M32)
    if v[0] == "l":
        return ("l", v[1], v[2] + sh, (v[3] << sh) & M32) if v[2] + sh < 32 else None
    if v[0] == "t" and v[7] is None and v[1] < 4 and sh < 24:
        return v[:6] + (v[6] + sh, None)
    return None


class _State:
    def __init__(self):
        self.regs = {r: ("l", r, 0, 0) for r in range(32)}
        self.next_sid = 32
        self.cr = {}       # CR field -> (signed, value, immediate, compare address, compared register)
        self.ctr = None
        self.bounds = {}   # symbol -> [(c, count, source, register, address)]

    def fresh(self, bound=None, source=None, reg=None, addr=None):
        sid = self.next_sid
        self.next_sid += 1
        if bound:
            self.bounds.setdefault(sid, []).append((0, bound, source, reg, addr))
        return ("l", sid, 0, 0)

    def forget_all(self):
        for r in range(32):
            self.regs[r] = self.fresh()
        self.cr.clear()
        self.ctr = None

    def call(self):
        for r in VOLATILE:
            self.regs[r] = self.fresh()
        for f in (0, 1, 5, 6, 7):
            self.cr.pop(f, None)
        self.ctr = None


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
        self.function_starts = sorted({s for s, _ in self.functions})
        self.function_starts_list = [s for s, _ in self.functions]
        self._branches = None   # sorted (target, source) of every b / bc without link in code
        self._labels = []       # sorted labels of the tables found in the first pass
        self._table_data = []   # (start, end) of the tables found in the first pass

    # -- image access -------------------------------------------------------------------------

    def word(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.image):
            return None
        return struct.unpack_from(">I", self.image, off)[0]

    def read(self, addr, size, signed=False):
        off = addr - self.base
        if off < 0 or off + size > len(self.image):
            return None
        return int.from_bytes(self.image[off:off + size], "big", signed=signed)

    def mapped(self, addr):
        return 0 <= addr - self.base < len(self.image)

    def in_code(self, addr):
        return any(s <= addr < e for s, e in self.code)

    def enclosing_function(self, addr):
        import bisect
        i = bisect.bisect_right(self.function_starts_list, addr) - 1
        for j in range(i, max(i - 4, -1), -1):        # .pdata entries do not overlap; look a little back anyway
            s, e = self.functions[j]
            if s <= addr < e:
                return s, e
        return None

    # -- join points ------------------------------------------------------------------------------

    def branches(self):
        if self._branches is None:
            found = []
            for s, e in self.code:
                off = s - self.base
                end = min(e - self.base, len(self.image)) & ~3
                for i, (w,) in enumerate(struct.iter_unpack(">I", self.image[off:end])):
                    op = w >> 26
                    if (op == 18 or op == 16) and not w & 1:
                        src = s + 4 * i
                        found.append((branch_target(src, w), src))
            found.sort()
            self._branches = found
        return self._branches

    def join_points(self, start, bctr):
        """Addresses in (start, bctr] where another path can enter: branch targets from the enclosing
        function (or +-64 KB without .pdata), table labels and function starts."""
        import bisect
        fn = self.enclosing_function(bctr) or (bctr - 0x10000, bctr + 0x10000)
        out = set()
        br = self.branches()
        i = bisect.bisect_right(br, (start, 0xFFFFFFFF))
        while i < len(br) and br[i][0] <= bctr:
            target, src = br[i]
            if fn[0] <= src < fn[1] and not any(lo <= src < hi for lo, hi in self._table_data):
                out.add(target)
            i += 1
        for seq in (self._labels, self.function_starts):
            j = bisect.bisect_right(seq, start)
            while j < len(seq) and seq[j] <= bctr:
                out.add(seq[j])
                j += 1
        return sorted(out)

    # -- symbolic execution -----------------------------------------------------------------------

    def region_start(self, bctr):
        """First address of the straight-line code that ends at the bctr (at most --window instructions)."""
        fn = self.enclosing_function(bctr)
        a = bctr
        while a - 4 >= bctr - 4 * self.window:
            if fn and a == fn[0]:
                break
            w = self.word(a - 4)
            if w is None or not self.in_code(a - 4) or not falls_through(w):
                break
            a -= 4
        return a

    def emulate(self, start, bctr, assume=None):
        st = _State()
        for sid, (c, count) in (assume or {}).items():
            st.bounds[sid] = [(c, count, "assumed", None, start)]
        for a in range(start, bctr, 4):
            w = self.word(a)
            if w is None:
                st.forget_all()
                continue
            self._step(st, a, w)
        return st

    def _max_value(self, st, v):
        """The largest unsigned value v can have, when the bounds make it known."""
        if v[0] == "c":
            return v[1]
        if v[0] == "l":
            best = None
            for cb, n, *_ in st.bounds.get(v[1], ()):
                lo = (-cb) & M32                     # S runs over lo .. lo + n - 1
                if lo + n - 1 > M32:
                    continue
                for c in (v[3], v[3] - (1 << 32)):    # the offset read as unsigned, then as negative
                    first, last = (lo << v[2]) + c, ((lo + n - 1) << v[2]) + c
                    if first >= 0 and last <= M32:
                        best = last if best is None else min(best, last)
                        break
            return best
        if v[0] == "t" and v[7] is None and not v[2]:
            return ((1 << (8 * v[1])) - 1) << v[6]
        return None

    def _rlwinm(self, st, v, sh, mb, me, ra, a):
        if v[0] == "c":
            r = ((v[1] << sh) | (v[1] >> (32 - sh))) & M32 if sh else v[1]
            return ("c", r & _mask32(mb, me))
        if me != 31 - sh:
            return st.fresh()
        if mb == 0:
            return _shift(v, sh) or st.fresh()          # slwi (or a copy when sh == 0)
        mx = self._max_value(st, v)
        if mx is not None and (mx << sh) < (1 << (32 - mb)):
            return _shift(v, sh) or st.fresh()          # the mask keeps every bit the value can have
        bits = 32 - mb - sh                              # otherwise: a new value below 2^bits, shifted
        if 0 < bits <= 12:
            return _shift(st.fresh(1 << bits, "mask", ra, a), sh) or st.fresh()
        return st.fresh()

    def _load(self, st, addr, size, signed, rd, a):
        if addr is not None:
            if addr[0] == "l" and (1 << addr[2]) == size and self.mapped(addr[3]):
                return ("t", size, signed, addr[1], addr[2], addr[3], 0, None)
            if addr[0] == "t" and size == 4 and addr[7] is not None and addr[6] == 2 and addr[1] < 4:
                return ("n", addr)
        if size == 1 and not signed:
            return st.fresh(256, "lbz", rd, a)
        return st.fresh()

    def _bound(self, st, bo, bi, a):
        """Records the index range a conditional branch implies on its not-taken path."""
        if bo & 0x10 or not bo & 4:
            return                      # no condition, or a CTR test too (the not-taken path proves nothing)
        cmp = st.cr.get(bi >> 2)
        if not cmp:
            return
        signed, val, imm, _caddr, creg = cmp
        if val[0] != "l" or val[2] != 0:
            return
        bit, fall = bi & 3, 1 - ((bo >> 3) & 1)
        if bit == 1 and fall == 0:      # not greater: value <= imm
            count = imm + 1
        elif bit == 0 and fall == 1:    # less: value < imm
            count = imm
        else:
            return
        if count >= 1:
            st.bounds.setdefault(val[1], []).append((val[3], count, "cmp-signed" if signed else "cmp", creg, a))

    def _step(self, st, a, w):
        regs = st.regs
        op = w >> 26
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        simm, uimm = sext16(w & 0xFFFF), w & 0xFFFF
        if op in (14, 15):                                       # addi / li, addis / lis
            c = simm if op == 14 else simm << 16
            regs[rd] = ("c", c & M32) if ra == 0 else (_add_const(regs[ra], c) or st.fresh())
        elif op in (12, 13):                                     # addic, addic.
            regs[rd] = _add_const(regs[ra], simm) or st.fresh()
            if op == 13:
                st.cr.pop(0, None)
        elif op == 7:                                            # mulli
            v = regs[ra]
            if v[0] == "c":
                regs[rd] = ("c", (v[1] * simm) & M32)
            elif simm > 0 and simm & (simm - 1) == 0:
                regs[rd] = _shift(v, simm.bit_length() - 1) or st.fresh()
            else:
                regs[rd] = st.fresh()
        elif op == 8:                                            # subfic
            v = regs[ra]
            regs[rd] = ("c", (simm - v[1]) & M32) if v[0] == "c" else st.fresh()
        elif op in (10, 11):                                     # cmplwi, cmpwi
            if rd & 1:
                st.cr.pop(rd >> 2, None)
            else:
                st.cr[rd >> 2] = (op == 11, regs[ra], uimm if op == 10 else simm, a, ra)
        elif op in (24, 25, 26, 27):                             # ori oris xori xoris
            v, imm = regs[rd], uimm if op in (24, 26) else uimm << 16
            if imm == 0:
                regs[ra] = v
            elif v[0] == "c":
                regs[ra] = ("c", (v[1] | imm) if op in (24, 25) else (v[1] ^ imm))
            else:
                regs[ra] = st.fresh()
        elif op in (28, 29):                                     # andi. andis.
            v, imm = regs[rd], uimm if op == 28 else uimm << 16
            st.cr.pop(0, None)
            if v[0] == "c":
                regs[ra] = ("c", v[1] & imm)
            elif op == 28 and imm and imm & (imm + 1) == 0:
                regs[ra] = self._rlwinm(st, v, 0, 32 - imm.bit_length(), 31, ra, a)
            else:
                regs[ra] = st.fresh()
        elif op == 21:                                           # rlwinm
            if w & 1:
                st.cr.pop(0, None)
            regs[ra] = self._rlwinm(st, regs[rd], rb, (w >> 6) & 31, (w >> 1) & 31, ra, a)
        elif op == 30:                                           # rldicl / rldicr / rldic (low 32 bits)
            if w & 1:
                st.cr.pop(0, None)
            xo, v = (w >> 2) & 7, regs[rd]
            sh = rb | (((w >> 1) & 1) << 5)
            m = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
            res = None
            if xo == 0 and sh == 0:
                res = v if m <= 32 else self._rlwinm(st, v, 0, m - 32, 31, ra, a)
            elif xo == 1 and m == 63 - sh and sh < 32:
                res = self._rlwinm(st, v, sh, 0, 31 - sh, ra, a)
            elif xo == 2 and sh < 32:
                res = self._rlwinm(st, v, sh, max(m - 32, 0), 31 - sh, ra, a)
            regs[ra] = res or st.fresh()
        elif op in (32, 33, 34, 35, 40, 41, 42, 43):             # lwz lbz lhz lha (and update forms)
            size = 4 if op in (32, 33) else 1 if op in (34, 35) else 2
            update = op & 1 == 1
            addr = ("c", simm & M32) if ra == 0 and not update else _add_const(regs[ra], simm)
            val = self._load(st, addr, size, op in (42, 43), rd, a)
            if update:
                regs[ra] = addr or st.fresh()
            regs[rd] = val
        elif op in (37, 39, 45, 49, 51, 53, 55):                 # stores and FP loads with update
            regs[ra] = _add_const(regs[ra], simm) or st.fresh()
        elif op == 46:                                           # lmw
            for r in range(rd, 32):
                regs[r] = st.fresh()
        elif op == 58:                                           # ld / ldu / lwa
            ds = sext16(w & 0xFFFC)
            addr = ("c", ds & M32) if ra == 0 else _add_const(regs[ra], ds)
            val = self._load(st, addr, 4, True, rd, a) if (w & 3) == 2 else st.fresh()
            if (w & 3) == 1:
                regs[ra] = addr or st.fresh()
            regs[rd] = val
        elif op == 62:                                           # std / stdu
            if (w & 3) == 1:
                regs[ra] = _add_const(regs[ra], sext16(w & 0xFFFC)) or st.fresh()
        elif op in (2, 3, 36, 38, 44, 47, 48, 50, 52, 54):       # traps, stores, FP loads
            pass
        elif op in (4, 5, 6):                                    # VMX / VMX128 (vector compares set CR6)
            st.cr.pop(6, None)
        elif op in (59, 63):                                     # FP arithmetic
            if w & 1:
                st.cr.pop(1, None)
            if op == 63 and ((w >> 1) & 0x3FF) in (0, 32, 64):
                st.cr.pop(rd >> 2, None)
        elif op == 16:                                           # bc
            if not rd & 4:
                st.ctr = None
            if w & 1:
                st.call()
            else:
                self._bound(st, rd, ra, a)
        elif op == 18:                                           # bl (b ends the analysed code)
            if w & 1:
                st.call()
        elif op == 19:
            xo = (w >> 1) & 0x3FF
            if xo in (16, 528):                                  # conditional blr / bctr, or a call
                if not rd & 4:
                    st.ctr = None
                if w & 1:
                    st.call()
                else:
                    self._bound(st, rd, ra, a)
            elif xo == 150:                                      # isync
                pass
            elif xo in (0, 33, 129, 193, 225, 257, 289, 417, 449):
                st.cr.clear()                                    # CR field and bit operations
            else:
                st.forget_all()
        elif op == 31:
            self._step31(st, a, w)
        else:
            st.forget_all()                                      # sc and anything not modelled

    def _step31(self, st, a, w):
        regs = st.regs
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        xo = (w >> 1) & 0x3FF
        if w & 1 or xo in (150, 214):
            st.cr.pop(0, None)
        if xo == 467:                                            # mtspr (mtctr)
            if (ra | (rb << 5)) == 9:
                st.ctr = regs[rd]
        elif xo == 144:                                          # mtcrf
            st.cr.clear()
        elif xo in (0, 32):                                      # cmpw / cmplw
            v = regs[rb]
            if rd & 1 or v[0] != "c":
                st.cr.pop(rd >> 2, None)
            else:
                imm = v[1] - (1 << 32) if xo == 0 and v[1] & 0x80000000 else v[1]
                st.cr[rd >> 2] = (xo == 0, regs[ra], imm, a, ra)
        elif xo == 444:                                          # or / mr
            vs, vb = regs[rd], regs[rb]
            if rd == rb:
                regs[ra] = vs
            elif vs[0] == "c" and vb[0] == "c":
                regs[ra] = ("c", vs[1] | vb[1])
            else:
                regs[ra] = st.fresh()
        elif xo in (266, 778):                                   # add
            regs[rd] = _add(regs[ra], regs[rb]) or st.fresh()
        elif xo in (40, 552):                                    # subf: rD = rB - rA
            va = regs[ra]
            regs[rd] = (_add_const(regs[rb], -va[1]) if va[0] == "c" else None) or st.fresh()
        elif xo in (104, 616):                                   # neg
            v = regs[ra]
            regs[rd] = ("c", (-v[1]) & M32) if v[0] == "c" else st.fresh()
        elif xo in (23, 55, 87, 119, 279, 311, 343, 375, 341, 373):   # indexed loads
            size = 4 if xo in (23, 55, 341, 373) else 1 if xo in (87, 119) else 2
            update = xo in (55, 119, 311, 375, 373)
            addr = _add(regs[ra] if ra or update else ("c", 0), regs[rb])
            val = self._load(st, addr, size, xo in (343, 375, 341, 373), rd, a)
            if update:
                regs[ra] = addr or st.fresh()
            regs[rd] = val
        elif xo == 986:                                          # extsw: the low 32 bits stay
            regs[ra] = regs[rd]
        elif xo in (922, 954):                                   # extsh, extsb
            bits, v = (16 if xo == 922 else 8), regs[rd]
            if v[0] == "c":
                x = v[1] & ((1 << bits) - 1)
                regs[ra] = ("c", (x - (1 << bits) if x >> (bits - 1) else x) & M32)
            elif v[0] == "t" and v[1] * 8 == bits and v[6] == 0 and v[7] is None:
                regs[ra] = ("t", v[1], True) + v[3:]
            else:
                mx = self._max_value(st, v)
                regs[ra] = v if mx is not None and mx < (1 << (bits - 1)) else st.fresh()
        elif xo == 28:                                           # and
            vs, vb = regs[rd], regs[rb]
            regs[ra] = ("c", vs[1] & vb[1]) if vs[0] == "c" and vb[0] == "c" else st.fresh()
        else:
            writes = gpr_writes(w)
            if writes is None:
                st.forget_all()
            else:
                for r in writes:
                    regs[r] = st.fresh()

    # -- one dispatch -------------------------------------------------------------------------------

    def _byte_load_feeds(self, bctr):
        """The previous finder's byte-switch test: an `lbz` close before the bctr, its result kept."""
        for k in range(2, 24):
            a = bctr - 4 * k
            i = self.word(a)
            if i is None or is_branch(i) or i == BCTR:
                return False
            if (i >> 26) == 34:
                rd = (i >> 21) & 31
                return not any(rd in _writes_or_all(self.word(x)) for x in range(a + 4, bctr, 4))
        return False

    def _interpret(self, st, bctr):
        t = st.ctr
        if t is None or t[0] not in ("t", "n"):
            cmp_bounds = any(b[2].startswith("cmp") for bl in st.bounds.values() for b in bl)
            if cmp_bounds or self._byte_load_feeds(bctr):
                what = ("no mtctr precedes the bctr in the analysed code" if t is None else
                        "the CTR value is a constant" if t[0] == "c" else
                        "the CTR value is not read from a table indexed by a bounded value")
                return {"ok": False, "class": "unresolved", "bctr": bctr,
                        "kind": "bounded" if cmp_bounds else "byte-switch",
                        "reason": f"dispatch sequence not recognised: {what}"}
            return {"ok": False, "class": "other", "bctr": bctr}
        levels = 2 if t[0] == "n" else 1
        inner = t[1] if levels == 2 else t
        _, size, signed, sid, k, a0, shift, base = inner
        cands = st.bounds.get(sid, [])
        if not cands:
            return {"ok": False, "class": "unbounded", "bctr": bctr, "table": a0, "entry_size": size,
                    "levels": levels}
        # Every bound holds on this path; the tightest that gives a valid table wins (fewest dead cases).
        rank = {"cmp": 0, "cmp-signed": 0, "mask": 1, "lbz": 2}
        first_failure = None
        for n, cand in enumerate(sorted(cands, key=lambda c: (rank[c[2]], c[1]))):
            res = self._table(st, bctr, t, levels, inner, cand)
            if res["ok"]:
                return res
            first_failure = first_failure or res
        return first_failure

    def _table(self, st, bctr, t, levels, inner, bound):
        _, size, signed, sid, k, a0, shift, base = inner
        cb, count, source, breg, baddr = bound
        kind = "byte-switch" if source == "lbz" else "bounded"

        def fail(reason):
            return {"ok": False, "class": "unresolved", "bctr": bctr, "kind": kind, "reason": reason}

        if count < 2:
            return fail(f"the bounds check allows {count} index value(s)")
        if count > self.max_entries:
            return fail(f"the bounds check allows {count} entries (more than --max-entries {self.max_entries})")
        table = (a0 - (cb << k)) & M32
        want = ("l", sid, 0, cb & M32)
        holding = [r for r in range(32) if st.regs[r] == want]
        scale = 0
        if breg in holding:
            reg = breg
        elif holding:
            reg = holding[0]
        else:
            # Only a scaled copy survives (rlwinm rI, rI, k): switch on it and stretch the label list
            # by 2^k, repeating each label into the unreachable in-between case numbers.
            scaled = sorted((v[2], r) for r, v in st.regs.items()
                            if v[0] == "l" and v[1] == sid and v[2] > 0 and v[3] == (cb << v[2]) & M32)
            if not scaled or count << scaled[0][0] > self.max_entries:
                return fail("no register holds the unscaled index at the bctr (scaled in place)")
            scale, reg = scaled[0]
        labels, second = [], []
        for n in range(count):
            p = (table + (n << k)) & M32
            raw = self.read(p, size, signed)
            if raw is None:
                return fail(f"table entry {n} at 0x{p:08X} is outside the image")
            if levels == 2:
                q = (base + (raw << shift)) & M32
                second.append(q)
                label = self.read(q, 4)
                if label is None:
                    return fail(f"second-level entry 0x{q:08X} is outside the image")
            elif base is None:
                if size != 4 or shift:
                    return fail("the table holds offsets but no base address is added to them")
                label = raw & M32
            else:
                label = (base + (raw << shift)) & M32
            labels.append(label)
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
        if len(labels) < 2 or any(label & 3 or not self.in_code(label) for label in labels):
            return fail("labels outside code")
        data = [(table, table + len(labels) * size)]
        if second:
            data.append((min(second), max(second) + 4))
        if scale:
            labels = [label for label in labels for _ in range(1 << scale)]
        return {"ok": True, "bctr": bctr, "kind": kind, "register": reg, "entry_size": size, "table": table,
                "labels": labels, "scale": scale, "bound": source, "levels": levels, "signed_entries": signed,
                "_inner": inner, "_bound_addr": baddr, "_data": data}

    @staticmethod
    def _same_dispatch(st, res):
        """Whether the state at the bctr reads the table entry res describes for the switch register."""
        t = st.ctr
        if t is None or t[0] not in ("t", "n") or (t[0] == "n") != (res["levels"] == 2):
            return False
        inner, main = (t[1] if t[0] == "n" else t), res["_inner"]
        if (inner[1], inner[2], inner[4], inner[6], inner[7]) != (main[1], main[2], main[4], main[6], main[7]):
            return False
        sid, k, a0 = inner[3], inner[4], inner[5]
        r, s = st.regs[res["register"]], res["scale"]
        if r[0] != "l" or r[1] != sid or r[2] != s or r[3] & ((1 << s) - 1) or k < s:
            return False
        return (a0 - ((r[3] >> s) << k)) & M32 == res["table"]

    def _consistent_from(self, j, bctr, res):
        """Whether a path entering at j reads the same table entry for the same switch register value."""
        st = self.emulate(j, bctr)
        if self._same_dispatch(st, res):
            return True
        r = st.regs[res["register"]]
        if res["scale"] or r[0] != "l" or r[1] >= 32 or r[2]:
            return False
        # Like the path through the bounds check, assume the switch value is a case number: masks and
        # extensions of the index are then exact on this path too.
        st = self.emulate(j, bctr, assume={r[1]: (r[3], len(res["labels"]))})
        return self._same_dispatch(st, res)

    def analyse(self, bctr):
        start = self.region_start(bctr)
        restarted = None
        while True:
            res = self._interpret(self.emulate(start, bctr), bctr)
            res["start"] = start
            if not res["ok"]:
                if restarted is not None and res["class"] == "unresolved":
                    res["reason"] += (f" (the analysis starts at 0x{restarted:08X}: another path enters the code"
                                      f" there, so the facts established before it do not hold)")
                elif restarted is not None:
                    res = {"ok": False, "class": "unresolved", "bctr": bctr, "kind": "bounded", "start": start,
                           "reason": (f"another path enters the dispatch code at 0x{restarted:08X}, and from there"
                                      f" the target is not read from a bounded table")}
                return res
            joins = self.join_points(start, bctr)
            before = [j for j in joins if j <= res["_bound_addr"]]
            if before:
                start = restarted = max(before)
                continue
            for j in joins:
                if not self._consistent_from(j, bctr, res):
                    return {"ok": False, "class": "unresolved", "bctr": bctr, "kind": res["kind"], "start": start,
                            "reason": (f"another path enters the dispatch at 0x{j:08X}, after the bounds check, and"
                                       f" does not read the same table entry for the switch register")}
            return res

    def run(self):
        sites = []
        for s, e in self.code:
            off = s - self.base
            end = min(e - self.base, len(self.image)) & ~3
            sites += [s + 4 * i for i, (w,) in enumerate(struct.iter_unpack(">I", self.image[off:end])) if w == BCTR]
        # Pass 1 learns every table's labels and data; pass 2 counts those labels as join points
        # and ignores "branches" decoded from table data.
        first = [self.analyse(a) for a in sites]
        # A byte switch's length is a guess (cut where its labels leave the function), so only tables
        # with a known length contribute labels.
        self._labels = sorted({label for r in first if r["ok"] and r["kind"] != "byte-switch" for label in r["labels"]})
        self._table_data = sorted(d for r in first if r["ok"] for d in r["_data"])
        results = [self.analyse(a) for a in sites]
        found = [r for r in results if r["ok"]]
        unresolved = [r for r in results if not r["ok"] and r["class"] == "unresolved"]
        unbounded = [r for r in results if not r["ok"] and r["class"] == "unbounded"]
        other = sum(1 for r in results if not r["ok"] and r["class"] == "other")
        return found, unresolved, other, unbounded

    def sequence(self, start, bctr, limit=40):
        """Disassembled dispatch code for the report (local diagnostics only)."""
        first = max(start, bctr - 4 * (limit - 1))
        return [f"0x{a:08X}  {disasm(a, self.word(a))}" for a in range(first, bctr + 4, 4)]

    def describe_unbounded(self, r):
        """How the first entries of a table read without a bounds check look (no game bytes)."""
        fn = self.enclosing_function(r["bctr"])
        inside = starts = 0
        if r["entry_size"] == 4 and r["levels"] == 1:
            for n in range(16):
                v = self.word((r["table"] + 4 * n) & M32)
                if v is None:
                    break
                inside += bool(fn and fn[0] <= v < fn[1])
                starts += v in self.function_starts
        return {"first16_in_function": inside, "first16_function_starts": starts}


# ---------------------------------------------------------------------------- output

def write_toml(path, found, source_name, digest):
    lines = [f"# Generated by recomp/tools/find_switch_tables.py from {source_name} (SHA-1 {digest}).",
             f"# {len(found)} switch tables: address = bctr, register = index register, labels in index order.",
             "# Include from the codegen config: includes = [\"<this file>\"]. Addresses only; no game bytes.", ""]
    for t in found:
        note = f"# {t['kind']}, {t['entry_size']}-byte entries at 0x{t['table']:08X}"
        if t.get("levels", 1) == 2:
            note += ", each selecting a word of a second table"
        if t.get("bound") == "mask":
            note += "; the index range comes from a mask"
        elif t.get("bound") == "cmp-signed":
            note += "; signed bounds check"
        if t["scale"]:
            note += f"; the index register is scaled by {1 << t['scale']} at the bctr, labels stretched to match"
        lines += ["[[switch_tables]]", f"address = 0x{t['bctr']:08X}", f"register = {t['register']}",
                  "labels = [" + ", ".join(f"0x{label:08X}" for label in t["labels"]) + "]", note, ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def _public(d):
    return {k: v for k, v in d.items() if not k.startswith("_")}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--base", help="load address for a raw PE image (hex); omit for an XEX2 file")
    ap.add_argument("--toml")
    ap.add_argument("--json")
    ap.add_argument("--window", type=int, default=WINDOW, help="instructions analysed before each bctr (default %(default)s)")
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
    found, unresolved, other, unbounded = finder.run()
    kinds = {}
    for t in found:
        kinds[t["entry_size"]] = kinds.get(t["entry_size"], 0) + 1
    print(f"{args.image}: {len(found)} switch tables ({', '.join(f'{v} with {k}-byte entries' for k, v in sorted(kinds.items())) or 'none'}), "
          f"{len(unresolved)} dispatches with a bounds check but unrecognised sequence, {other + len(unbounded)} other bctr sites")
    extra = [f"{sum(1 for t in found if t.get('levels') == 2)} two-level",
             f"{sum(1 for t in found if t.get('bound') == 'mask')} bounded by a mask",
             f"{sum(1 for t in found if t['scale'])} scaled in place"]
    print(f"  of the tables: {', '.join(extra)}; {len(unbounded)} bctr sites read a table without a bounds check (not switches here)")
    for u in unresolved[:20]:
        print(f"  unresolved bctr 0x{u['bctr']:08X} ({u['kind']}: {u.get('reason', '?')})")
    if args.toml:
        write_toml(args.toml, found, args.image.replace("\\", "/").split("/")[-1], digest)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"image": args.image, "sha1": digest, "base": f"0x{base:08X}", "window": args.window,
                       "found": len(found), "unresolved": len(unresolved), "other_bctr": other + len(unbounded),
                       "unbounded": len(unbounded),
                       "tables": [{**_public(t), "bctr": f"0x{t['bctr']:08X}", "table": f"0x{t['table']:08X}",
                                   "start": f"0x{t['start']:08X}", "labels": [f"0x{label:08X}" for label in t["labels"]]}
                                  for t in found],
                       "unresolved_sites": [{"bctr": f"0x{u['bctr']:08X}", "kind": u["kind"], "reason": u.get("reason", ""),
                                             "sequence": finder.sequence(u["start"], u["bctr"])}
                                            for u in unresolved],
                       "unbounded_sites": [{"bctr": f"0x{u['bctr']:08X}", "table": f"0x{u['table']:08X}",
                                            "entry_size": u["entry_size"], "levels": u["levels"],
                                            **finder.describe_unbounded(u)} for u in unbounded]},
                      f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
