"""Tests for recomp/tools/find_switch_tables.py.

Every table the finder reports is checked by execution: a small PowerPC interpreter (the subset the
generated dispatch code uses) runs the dispatch for each index value, and the switch register's value at
the `bctr` must select the label the CTR holds. A seeded generator builds dispatch functions in many
shapes (TTT2's scheduling, early table address, bias folded into the table, copies, masks, byte loads,
64-bit shifts, `add` + D-form loads, offset and two-level tables, unrelated instructions) and paths that
enter the dispatch code from elsewhere; the finder must find the supported shapes and must never report
a table that is wrong for any path. The instruction encoders are checked against GNU as output.
"""
import importlib.util
import os
import random
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
_SPEC = importlib.util.spec_from_file_location("find_switch_tables",
                                               os.path.join(ROOT, "recomp", "tools", "find_switch_tables.py"))
FST = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(FST)

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
BASE, TEXT, RDATA, PDATA = 0x82000000, 0x82010000, 0x82100000, 0x82110000
SIZE = 0x120000


# ---------------------------------------------------------------------------- encoders

def _d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def _x(xo, rt, ra, rb, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def _md(xo, ra, rs, sh, m):
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((m & 31) << 6) | ((m >> 5) << 5)
            | (xo << 2) | ((sh >> 5) << 1))


def li(rt, v): return _d(14, rt, 0, v)
def lis(rt, v): return _d(15, rt, 0, v)
def addi(rt, ra, v): return _d(14, rt, ra, v)
def ori(ra, rs, v): return _d(24, rs, ra, v)
def andi_(ra, rs, v): return _d(28, rs, ra, v)
def mulli(rt, ra, v): return _d(7, rt, ra, v)
def cmplwi(crf, ra, v): return _d(10, crf << 2, ra, v)
def cmpwi(crf, ra, v): return _d(11, crf << 2, ra, v)
def lwz(rt, d, ra): return _d(32, rt, ra, d)
def lwzu(rt, d, ra): return _d(33, rt, ra, d)
def lbz(rt, d, ra): return _d(34, rt, ra, d)
def lhz(rt, d, ra): return _d(40, rt, ra, d)
def lha(rt, d, ra): return _d(42, rt, ra, d)
def stw(rs, d, ra): return _d(36, rs, ra, d)
def rlwinm(ra, rs, sh, mb, me): return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)
def slwi(ra, rs, n): return rlwinm(ra, rs, n, 0, 31 - n)
def clrlwi(ra, rs, n): return rlwinm(ra, rs, 0, n, 31)
def rldicl(ra, rs, sh, mb): return _md(0, ra, rs, sh, mb)
def rldicr(ra, rs, sh, me): return _md(1, ra, rs, sh, me)
def rldic(ra, rs, sh, mb): return _md(2, ra, rs, sh, mb)
def lwzx(rt, ra, rb): return _x(23, rt, ra, rb)
def lbzx(rt, ra, rb): return _x(87, rt, ra, rb)
def lhzx(rt, ra, rb): return _x(279, rt, ra, rb)
def lhax(rt, ra, rb): return _x(343, rt, ra, rb)
def add(rt, ra, rb): return _x(266, rt, ra, rb)
def subf(rt, ra, rb): return _x(40, rt, ra, rb)
def xor(ra, rs, rb): return _x(316, rs, ra, rb)
def mr(ra, rs): return _x(444, rs, ra, rs)
def extsb(ra, rs): return _x(954, rs, ra, 0)
def extsh(ra, rs): return _x(922, rs, ra, 0)
def extsw(ra, rs): return _x(986, rs, ra, 0)
def mtctr(rs): return (31 << 26) | (rs << 21) | (9 << 16) | (467 << 1)
def bc(bo, bi, disp): return (16 << 26) | (bo << 21) | (bi << 16) | (disp & 0xFFFC)
def b(disp): return (18 << 26) | (disp & 0x03FFFFFC)
def bl(disp): return b(disp) | 1
def bgtlr(crf): return (19 << 26) | (12 << 21) | ((crf * 4 + 1) << 16) | (16 << 1)


NOP, BLR, BCTR = 0x60000000, 0x4E800020, 0x4E800420
BGT, BGE, BLE, BLT = (12, 1), (4, 0), (4, 1), (12, 0)   # (BO, CR bit) of the bounds branches


def ha(v): return ((v + 0x8000) >> 16) & 0xFFFF
def lo(v): return v & 0xFFFF


# ---------------------------------------------------------------------------- image and assembler

class Image:
    def __init__(self):
        self.mem = bytearray(SIZE)
        self.functions = []
        self.text_end = TEXT
        self.rdata = RDATA

    def put(self, addr, value, size=4):
        self.mem[addr - BASE:addr - BASE + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "big")

    def get(self, addr, size=4, signed=False):
        off = (addr & M32) - BASE
        if off < 0 or off + size > SIZE:
            raise IndexError(f"0x{addr & M32:08X}")
        return int.from_bytes(self.mem[off:off + size], "big", signed=signed)

    def alloc(self, nbytes, align=4):
        self.rdata = (self.rdata + align - 1) & ~(align - 1)
        addr = self.rdata
        self.rdata += nbytes
        return addr

    def finder(self, **kw):
        for i, (s, e) in enumerate(self.functions):
            self.put(PDATA + 8 * i, s)
            self.put(PDATA + 8 * i + 4, ((e - s) // 4) << 8)
        sections = [(".text", TEXT, max(self.text_end, TEXT + 4), True), (".rdata", RDATA, PDATA, False),
                    (".pdata", PDATA, PDATA + 8 * len(self.functions), False)]
        return FST.Finder(bytes(self.mem), BASE, sections, **kw)


class Asm:
    def __init__(self, img, org):
        self.img, self.pc, self.labels, self.fixups = img, org, {}, []

    def emit(self, *words):
        for w in words:
            self.img.put(self.pc, w)
            self.pc += 4

    def label(self, name):
        self.labels[name] = self.pc

    def bc(self, bo, bi, name):
        self.fixups.append((self.pc, "bc", (bo, bi), name))
        self.emit(0)

    def b(self, name):
        self.fixups.append((self.pc, "b", None, name))
        self.emit(0)

    def finish(self):
        for pc, kind, args, name in self.fixups:
            disp = self.labels[name] - pc
            self.img.put(pc, bc(args[0], args[1], disp) if kind == "bc" else b(disp))


# ---------------------------------------------------------------------------- interpreter

class Cpu:
    """Executes the subset of PowerPC the generated dispatch code uses (64-bit registers)."""

    def __init__(self, img, regs):
        self.img, self.r, self.cr, self.ctr = img, [v & M64 for v in regs], [0] * 8, 0

    def _cmp(self, crf, a, b_, signed):
        if signed:
            a, b_ = (a - (1 << 32) if a & 0x80000000 else a), (b_ - (1 << 32) if b_ & 0x80000000 else b_)
        self.cr[crf] = 8 if a < b_ else 4 if a > b_ else 2

    def _cond(self, bo, bi):
        if not bo & 4:
            self.ctr = (self.ctr - 1) & M64
            if bool(self.ctr != 0) == bool(bo & 2):
                return False
        if bo & 16:
            return True
        bit = (self.cr[bi >> 2] >> (3 - (bi & 3))) & 1
        return bit == ((bo >> 3) & 1)

    def run(self, pc, limit=400):
        """Runs until the bctr: ("bctr", pc), or ("return", r3) when the code returns first."""
        r = self.r
        for _ in range(limit):
            w = self.img.get(pc)
            op, rd, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            simm, uimm = FST.sext16(w & 0xFFFF), w & 0xFFFF
            nxt = pc + 4
            if w == BCTR:
                return ("bctr", pc)
            if w == BLR:
                return ("return", r[3])
            if op == 14:
                r[rd] = ((r[ra] if ra else 0) + simm) & M64
            elif op == 15:
                r[rd] = ((r[ra] if ra else 0) + (simm << 16)) & M64
            elif op == 7:
                r[rd] = (r[ra] * simm) & M64
            elif op == 24:
                r[ra] = r[rd] | uimm
            elif op == 28:
                r[ra] = r[rd] & uimm
                self._cmp(0, r[ra] & M32, 0, True)
            elif op in (10, 11):
                self._cmp(rd >> 2, r[ra] & M32, (uimm if op == 10 else simm) & M32, op == 11)
            elif op == 16:
                if self._cond(rd, ra):
                    nxt = FST.branch_target(pc, w)
            elif op == 18:
                nxt = FST.branch_target(pc, w)
            elif op == 19 and ((w >> 1) & 0x3FF) == 16:
                if self._cond(rd, ra):
                    return ("return", r[3])
            elif op == 21:
                sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
                v = r[rd] & M32
                rot = ((v << sh) | (v >> (32 - sh))) & M32 if sh else v
                r[ra] = rot & FST._mask32(mb, me)
            elif op == 30:
                xo = (w >> 2) & 7
                sh = rb | (((w >> 1) & 1) << 5)
                m = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
                v = r[rd]
                rot = ((v << sh) | (v >> (64 - sh))) & M64 if sh else v
                if xo == 0:
                    mask = M64 >> m
                elif xo == 1:
                    mask = (M64 << (63 - m)) & M64
                else:
                    mask = (M64 >> m) & ((M64 << sh) & M64)
                r[ra] = rot & mask
            elif op in (32, 33, 34, 40, 42):
                size = {32: 4, 33: 4, 34: 1, 40: 2, 42: 2}[op]
                ea = ((r[ra] if ra or op == 33 else 0) + simm) & M32
                r[rd] = self.img.get(ea, size, op == 42) & M64
                if op == 33:
                    r[ra] = ea
            elif op == 36:
                self.img.put(((r[ra] if ra else 0) + simm) & M32, r[rd] & M32)
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo == 467:
                    self.ctr = r[rd]
                elif xo in (23, 87, 279, 343):
                    size = {23: 4, 87: 1, 279: 2, 343: 2}[xo]
                    ea = ((r[ra] if ra else 0) + r[rb]) & M32
                    r[rd] = self.img.get(ea, size, xo == 343) & M64
                elif xo == 266:
                    r[rd] = (r[ra] + r[rb]) & M64
                elif xo == 40:
                    r[rd] = (r[rb] - r[ra]) & M64
                elif xo == 444:
                    r[ra] = r[rd] | r[rb]
                elif xo == 316:
                    r[ra] = r[rd] ^ r[rb]
                elif xo in (954, 922, 986):
                    bits = {954: 8, 922: 16, 986: 32}[xo]
                    v = r[rd] & ((1 << bits) - 1)
                    r[ra] = (v - (1 << bits) if v >> (bits - 1) else v) & M64
                else:
                    raise NotImplementedError(f"op31 xo {xo} at 0x{pc:08X}")
            elif w != NOP:
                raise NotImplementedError(f"0x{w:08X} at 0x{pc:08X}")
            pc = nxt
        raise RuntimeError("the dispatch code did not reach a bctr")


# ---------------------------------------------------------------------------- dispatch generator

POOL = list(range(3, 13)) + list(range(14, 32))
SCRATCH = RDATA + 0xF000


class Dispatch:
    """One generated dispatch function and what it must do."""


def build(img, rng, **opts):
    """Emits a function with one switch dispatch at img.text_end. Options default to random choices."""
    o = {
        "src": rng.choice(["arg"] * 3 + ["byte"]),
        "bound": None, "count": None, "bias": None, "copy": rng.random() < 0.2, "early": rng.random() < 0.3,
        "scale": rng.choice(["slwi", "slwi", "sldi", "mulli", "rldic", "clrlslwi", "inplace"]),
        "load": rng.choice(["x", "x", "d"]), "kind": rng.choice(["abs"] * 4 + ["byte", "half", "shalf", "two"]),
        "shift": rng.choice([0, 2]), "inline": rng.random() < 0.6, "noise": rng.randint(0, 6), "join": None,
        "index": None,
    }
    o.update(opts)
    if o["bound"] is None:
        o["bound"] = rng.choice(["bgt", "bgt", "bge", "bgtlr", "cmpwi", "mask"] + (["none"] if o["src"] == "byte" else []))
    if o["count"] is None:
        o["count"] = 1 << rng.randint(1, 5) if o["bound"] == "mask" else rng.randint(2, 24)
    if o["bias"] is None:
        o["bias"] = rng.choice([0, 0, rng.randint(1, 40)]) if o["bound"] in ("bgt", "bge", "bgtlr", "cmpwi") else 0
    if o["index"] is None:
        o["index"] = rng.choice(["x", "recompute", "fold"]) if o["bias"] else "x"
    if o["join"] in ("pre", "post", "bad"):
        o.update(src="arg", bias=0, copy=False, index="x")
        if o["bound"] in ("mask", "none"):
            o["bound"] = "bgt"
    if o["join"] == "post":
        o["early"] = False
    count, bias = o["count"], o["bias"]

    regs = rng.sample(POOL, len(POOL))
    v, x, c, t, s, l, bb, e, t2, p, y, m = regs[:12]
    noise_regs = regs[12:]
    if not o["bias"]:
        x = v
    crf = rng.randrange(8)
    size = {"abs": 4, "byte": 1, "half": 2, "shalf": 2, "two": 1}[o["kind"]]
    k = size.bit_length() - 1
    shift = o["shift"] if o["kind"] in ("byte", "half", "shalf") else 0

    start = img.text_end
    asm = Asm(img, start)
    seq = []                                   # ("i", word) | ("bc", bo, bi, label) | ("label", name)

    def i(*words):
        seq.extend(("i", w) for w in words)

    if o["src"] == "byte":
        i(lis(p, ha(SCRATCH)), addi(p, p, lo(SCRATCH)), lbz(v, 0, p))
    seq.append(("table_setup",))               # emitted here only for an early table address
    if bias:
        i(addi(x, v, -bias))
    cmpreg = x
    if o["copy"]:
        i(mr(c, x))
        cmpreg = c
    seq.append(("label", "pre"))
    index_reg = x
    if o["bound"] in ("bgt", "cmpwi"):
        i((cmplwi if o["bound"] == "bgt" else cmpwi)(crf, cmpreg, count - 1))
        seq.append(("bc", BGT[0], crf * 4 + BGT[1], "default"))
    elif o["bound"] == "bge":
        i(cmplwi(crf, cmpreg, count))
        seq.append(("bc", BGE[0], crf * 4 + BGE[1], "default"))
    elif o["bound"] == "bgtlr":
        i(cmplwi(crf, cmpreg, count - 1), bgtlr(crf))
    elif o["bound"] == "mask":
        i(clrlwi(m, x, 32 - (count.bit_length() - 1)))
        index_reg = m
    seq.append(("label", "post"))
    if not o["early"]:
        seq.append(("table_setup_late",))
    seq.append(("label", "bad"))
    fold_delta = 0
    if o["index"] == "recompute":
        i(addi(y, v, -bias))
        index_reg = y
    elif o["index"] == "fold":
        index_reg = v
        fold_delta = -bias
    elif o["copy"] and o["bound"] != "mask" and rng.random() < 0.5:
        index_reg = c
    # scale the index for the first-level entry size
    if k == 0:
        sreg = index_reg
    elif o["scale"] == "slwi":
        sreg = s
        i(slwi(s, index_reg, k))
    elif o["scale"] == "sldi":
        sreg = s
        i(rldicr(s, index_reg, k, 63 - k))
    elif o["scale"] == "mulli":
        sreg = s
        i(mulli(s, index_reg, 1 << k))
    elif o["scale"] == "rldic":
        sreg = s
        i(rldic(s, index_reg, k, 32))
    elif o["scale"] == "clrlslwi" and o["bound"] != "none":
        sreg = s
        i(rlwinm(s, index_reg, k, 24 - k, 31 - k))
    else:
        sreg = index_reg
        i(slwi(index_reg, index_reg, k))
    disp = rng.choice([0, 8, -4]) if o["load"] == "d" else 0
    first = {"abs": lwzx, "byte": lbzx, "half": lhzx, "shalf": lhax, "two": lbzx}[o["kind"]]
    dform = {"abs": lwz, "byte": lbz, "half": lhz, "shalf": lha, "two": lbz}[o["kind"]]
    dst = l if o["kind"] != "two" else e
    if o["load"] == "d":
        a = y if o["index"] != "recompute" else s if sreg != s else regs[12]
        if a in noise_regs:
            noise_regs.remove(a)
        i(add(a, *rng.sample([sreg, t], 2)), dform(dst, disp, a))
    else:
        i(first(dst, *rng.sample([sreg, t], 2)))
    if o["kind"] in ("byte", "half", "shalf"):
        if shift:
            i(slwi(l, l, shift))
        seq.append(("base_setup",))
        i(add(l, *rng.sample([l, bb], 2)))
    elif o["kind"] == "two":
        seq.append(("t2_setup",))
        i(slwi(e, e, 2), lwzx(l, *rng.sample([e, t2], 2)))
    i(mtctr(l))
    seq.append(("label", "ctr"))
    i(BCTR)

    # unrelated instructions between the dispatch instructions (never after the bctr)
    for _ in range(o["noise"]):
        if len(noise_regs) < 2:
            break
        n1, n2 = rng.sample(noise_regs, 2)
        w = rng.choice([li(n1, rng.randint(-500, 500)), addi(n1, n2, rng.randint(-50, 50)), xor(n1, n1, n2),
                        ori(n1, n2, rng.randint(0, 0xFFFF)), mulli(n1, n2, rng.randint(-9, 9))])
        seq.insert(rng.randint(0, len(seq) - 2), ("i", w))

    # layout: code, inline table, cases, default, alternate path; tables in .rdata unless inline
    n_cases = rng.randint(1, count) if o["kind"] != "two" else rng.randint(1, min(count, 12))
    # first pass: lay out to learn addresses (each setup is lis + addi)
    pc = start
    for it in seq:
        if it[0] in ("i", "bc"):
            pc += 4
        elif it[0] == "table_setup" and o["early"]:
            pc += 8
        elif it[0] == "table_setup_late":
            pc += 8
        elif it[0] in ("base_setup", "t2_setup"):
            pc += 8
    bctr_addr = pc - 4
    inline = o["inline"] and o["kind"] == "abs"
    table_addr = pc if inline else img.alloc(count * size + 8, 4)
    pc += count * 4 if inline else 0
    cases = []
    for n in range(n_cases):
        cases.append(pc)
        pc += 8
    default = pc
    pc += 8
    mapping = [rng.choice(cases) for _ in range(count)]
    base_addr = cases[len(cases) // 2] if o["kind"] == "shalf" else cases[0]
    t2_addr = img.alloc(4 * n_cases, 4) if o["kind"] == "two" else None
    if o["kind"] == "two":
        idx = [cases.index(lbl) for lbl in mapping]
    alt_table = img.alloc(count * size + 8, 4)

    # table contents
    for n, lbl in enumerate(mapping):
        if o["kind"] == "abs":
            img.put(table_addr + 4 * n, lbl)
            img.put(alt_table + 4 * n, cases[(cases.index(lbl) + 1) % len(cases)] if len(cases) > 1 else default)
        elif o["kind"] == "two":
            img.put(table_addr + n, idx[n], 1)
            img.put(alt_table + n, (idx[n] + 1) % n_cases, 1)
        else:
            off = (lbl - base_addr) >> shift
            img.put(table_addr + size * n, off, size)
            img.put(alt_table + size * n, ((default - base_addr) >> shift), size)
    if o["kind"] == "two":
        for j, lbl in enumerate(cases):
            img.put(t2_addr + 4 * j, lbl)

    setup_value = (table_addr + fold_delta * size - disp) & M32

    # emit
    for it in seq:
        if it[0] == "i":
            asm.emit(it[1])
        elif it[0] == "bc":
            asm.bc(it[1], it[2], it[3])
        elif it[0] == "label":
            asm.label(it[1])
        elif (it[0] == "table_setup" and o["early"]) or it[0] == "table_setup_late":
            asm.emit(lis(t, ha(setup_value)), addi(t, t, lo(setup_value)))
        elif it[0] == "base_setup":
            asm.emit(lis(bb, ha(base_addr)), addi(bb, bb, lo(base_addr)))
        elif it[0] == "t2_setup":
            asm.emit(lis(t2, ha(t2_addr)), addi(t2, t2, lo(t2_addr)))
    assert asm.pc == bctr_addr + 4, (hex(asm.pc), hex(bctr_addr))
    if inline:
        asm.pc += count * 4
    for n, at in enumerate(cases):
        asm.pc = at
        asm.emit(li(3, 1000 + n), BLR)
    asm.label("default")
    asm.emit(li(3, -1), BLR)
    alt_index = rng.randrange(count)
    alt = None
    if o["join"]:
        alt = asm.pc
        if o["join"] == "ctr":
            asm.emit(lis(l, ha(default)), addi(l, l, lo(default)), mtctr(l))
            asm.b("ctr")
        else:
            asm.emit(lis(v, ha(alt_index)), ori(v, v, alt_index))
            if o["join"] == "bad" or (o["join"] == "pre" and o["early"]):
                other = (alt_table + fold_delta * size - disp) & M32
                asm.emit(lis(t, ha(other)), addi(t, t, lo(other)))
            asm.b(o["join"])
    asm.finish()
    end = asm.pc
    img.functions.append((start, end))
    img.text_end = end
    d = Dispatch()
    d.__dict__.update(o)
    d.start, d.end, d.bctr, d.alt, d.alt_index = start, end, bctr_addr, alt, alt_index
    d.mapping, d.input_reg, d.default = mapping, v, default
    d.expect_found = not (o["join"] in ("bad", "ctr") or (o["join"] == "pre" and o["early"])
                          or (o["bound"] == "none" and o["src"] == "arg"))
    return d


def run_main_path(img, d, value, rng):
    regs = [rng.getrandbits(64) for _ in range(32)]
    if d.src == "byte":
        img.put(SCRATCH, value, 1)
    else:
        regs[d.input_reg] = value
    cpu = Cpu(img, regs)
    return cpu, cpu.run(d.start)


def check_table(test, img, d, table, rng):
    """Every in-range index (and the alternate path) must select the label the CTR holds."""
    labels = table["labels"]
    for n in range(d.count):
        cpu, res = run_main_path(img, d, n + d.bias, rng)
        test.assertEqual(res, ("bctr", d.bctr), f"{d.__dict__}: index {n} did not reach the bctr")
        test.assertEqual(cpu.ctr & M32, d.mapping[n])
        sw = cpu.r[table["register"]] & M32
        test.assertLess(sw, len(labels), f"{d.__dict__}: switch r{table['register']} = {sw} for index {n}")
        test.assertEqual(labels[sw], cpu.ctr & M32, f"{d.__dict__}: wrong label for index {n}")
    if d.alt is not None:
        cpu = Cpu(img, [rng.getrandbits(64) for _ in range(32)])
        res = cpu.run(d.alt)
        test.assertEqual(res, ("bctr", d.bctr))
        sw = cpu.r[table["register"]] & M32
        test.assertLess(sw, len(labels), f"{d.__dict__}: the alternate path's switch value {sw} is out of range")
        test.assertEqual(labels[sw], cpu.ctr & M32, f"{d.__dict__}: the alternate path reads another label")


def find(img, **kw):
    found, unresolved, other, unbounded = img.finder(**kw).run()
    return ({t["bctr"]: t for t in found}, {u["bctr"]: u for u in unresolved},
            {u["bctr"]: u for u in unbounded}, other)


class EncoderTests(unittest.TestCase):
    def test_encoders_match_gnu_as(self):
        # powerpc-none-elf-as -a32 -be -mregnames -mpower7 on the same instructions (session notes)
        words = [li(3, -5), lis(11, -32254), addi(11, 11, 1234), ori(11, 11, 0x8000), cmplwi(6, 3, 7),
                 cmpwi(7, 4, 9), bc(12, 25, 16), bc(4, 24, 16), bc(4, 29, -16), bc(12, 0, 8), bgtlr(6), b(64),
                 slwi(10, 3, 2), clrlwi(10, 3, 24), rlwinm(10, 3, 2, 22, 29), lwzx(11, 10, 11), lbzx(11, 10, 11),
                 lhzx(11, 10, 11), lhax(11, 10, 11), add(11, 11, 12), subf(11, 12, 11), mr(10, 3), mtctr(11),
                 lwz(11, 8, 10), lbz(9, -4, 31), lhz(9, 6, 31), lha(9, 6, 31), rldicr(10, 3, 2, 61),
                 rldicl(10, 3, 0, 32), rldic(10, 3, 2, 30), mulli(10, 3, 4), extsb(9, 9), extsh(9, 9), extsw(9, 9),
                 andi_(9, 3, 31), lwzu(9, 4, 11), BCTR, BLR, bl(8)]
        self.assertEqual(" ".join("%08X" % (w & M32) for w in words),
                         "3860FFFB 3D608202 396B04D2 616B8000 2B030007 2F840009 41990010 40980010 409DFFF0 "
                         "41800008 4D990020 48000040 546A103A 546A063E 546A15BA 7D6A582E 7D6A58AE 7D6A5A2E "
                         "7D6A5AAE 7D6B6214 7D6C5850 7C6A1B78 7D6903A6 816A0008 893FFFFC A13F0006 A93F0006 "
                         "786A1764 786A0020 786A1788 1D430004 7D290774 7D290734 7D2907B4 7069001F 852B0004 "
                         "4E800420 4E800020 48000009")

    def test_disassembly_of_a_dispatch(self):
        seq = [(cmplwi(6, 3, 7), "cmplwi cr6,r3,7"), (bc(12, 25, 16), "bgt cr6,0x82010014"),
               (lis(11, -32254), "lis r11,-32254"), (slwi(10, 3, 2), "rlwinm r10,r3,2,0,29"),
               (lwzx(11, 10, 11), "lwzx r11,r10,r11"), (mtctr(11), "mtctr r11"), (BCTR, "bctr"),
               (bgtlr(6), "bgtlr cr6"), (mr(10, 3), "mr r10,r3"), (rldicl(3, 3, 0, 32), "rldicl r3,r3,0,32")]
        for n, (w, text) in enumerate(seq):
            self.assertEqual(FST.disasm(0x82010004 if n == 1 else 0x82010000, w), text)


class ShapeTests(unittest.TestCase):
    """Named dispatch shapes, each checked by execution."""

    def one(self, seed=1, **opts):
        img, rng = Image(), random.Random(seed)
        d = build(img, rng, **opts)
        found, unresolved, unbounded, _ = find(img)
        return img, rng, d, found, unresolved, unbounded

    def found_ok(self, **opts):
        img, rng, d, found, unresolved, _ = self.one(**opts)
        self.assertIn(d.bctr, found, unresolved.get(d.bctr))
        check_table(self, img, d, found[d.bctr], rng)
        return found[d.bctr]

    def test_ttt2_order(self):
        t = self.found_ok(src="arg", bound="bgt", count=8, kind="abs", early=False, scale="slwi", load="x", noise=0)
        self.assertEqual((t["kind"], t["entry_size"], t["scale"], len(t["labels"])), ("bounded", 4, 0, 8))

    def test_bge_gives_max_entries(self):
        t = self.found_ok(src="arg", bound="bge", count=6, kind="abs", noise=2)
        self.assertEqual(len(t["labels"]), 6)

    def test_conditional_return_bound(self):
        self.found_ok(src="arg", bound="bgtlr", count=5, kind="abs")

    def test_table_address_set_before_the_bounds_check(self):
        self.found_ok(src="arg", bound="bgt", count=9, kind="abs", early=True, noise=3)

    def test_bias_folded_into_the_table_address(self):
        self.found_ok(src="arg", bound="bgt", count=7, bias=12, index="fold", kind="abs")

    def test_biased_index_recomputed(self):
        self.found_ok(src="arg", bound="bgt", count=7, bias=3, index="recompute", kind="abs", scale="inplace")

    def test_masks_shifts_and_multiplies(self):
        for scale in ("sldi", "mulli", "rldic", "clrlslwi", "inplace"):
            with self.subTest(scale=scale):
                self.found_ok(src="arg", bound="bgt", count=10, kind="abs", scale=scale)

    def test_add_then_d_form_load(self):
        for kind in ("abs", "byte", "half", "shalf", "two"):
            with self.subTest(kind=kind):
                self.found_ok(src="arg", bound="bgt", count=12, kind=kind, load="d")

    def test_offset_and_two_level_tables(self):
        for kind in ("byte", "half", "shalf", "two"):
            for shift in (0, 2):
                with self.subTest(kind=kind, shift=shift):
                    t = self.found_ok(src="arg", bound="bgt", count=11, kind=kind, shift=shift, load="x")
                    self.assertEqual(t["levels"], 2 if kind == "two" else 1)

    def test_bound_from_a_mask(self):
        t = self.found_ok(src="arg", bound="mask", count=16, kind="abs")
        self.assertEqual((t["bound"], len(t["labels"])), ("mask", 16))

    def test_byte_load_without_a_bounds_check(self):
        t = self.found_ok(src="byte", bound="none", count=9, kind="abs", inline=False)
        self.assertEqual(t["kind"], "byte-switch")

    def test_copy_of_the_index_compared(self):
        self.found_ok(src="arg", bound="bgt", count=6, copy=True, kind="abs")

    def test_join_before_the_bounds_check(self):
        self.found_ok(src="arg", bound="bgt", count=6, kind="abs", join="pre", early=False)

    def test_join_after_the_bounds_check_with_the_same_table(self):
        self.found_ok(src="arg", bound="bgt", count=6, kind="abs", join="post")

    def test_join_with_another_table_is_reported_not_guessed(self):
        for early in (False, True):
            with self.subTest(early=early):
                img, rng, d, found, unresolved, _ = self.one(src="arg", bound="bgt", count=6, kind="abs",
                                                              join="bad", early=early)
                self.assertNotIn(d.bctr, found)
                self.assertIn("another path enters", unresolved[d.bctr]["reason"])

    def test_join_at_the_bctr_is_reported(self):
        img, rng, d, found, unresolved, _ = self.one(src="arg", bound="bgt", count=6, kind="abs", join="ctr")
        self.assertNotIn(d.bctr, found)
        self.assertIn(d.bctr, unresolved)

    def test_join_before_the_compare_after_an_early_table_address(self):
        img, rng, d, found, unresolved, _ = self.one(src="arg", bound="bgt", count=6, kind="abs", join="pre",
                                                      early=True)
        self.assertNotIn(d.bctr, found)
        self.assertIn("another path enters", unresolved[d.bctr]["reason"])

    def test_table_read_without_a_bound_is_not_a_switch(self):
        img, rng, d, found, unresolved, unbounded = self.one(src="arg", bound="none", count=6, kind="abs")
        self.assertNotIn(d.bctr, found)
        self.assertIn(d.bctr, unbounded)


def _function(img, words):
    start = img.text_end
    for n, w in enumerate(words):
        img.put(start + 4 * n, w)
    img.text_end = start + 4 * len(words)
    img.functions.append((start, img.text_end))
    return start


class NegativeTests(unittest.TestCase):
    def table(self, img, labels):
        addr = img.alloc(4 * len(labels) + 8)
        for n, lbl in enumerate(labels):
            img.put(addr + 4 * n, lbl)
        return addr

    def dispatch(self, middle, bo=12, bit=1, imm=3, lead=0):
        """cmplwi cr6,r3,imm; bc -> default; lis/addi r11 = table; <middle>; slwi; lwzx; mtctr; bctr; cases.
        The table holds `lead` default labels, then the four case labels."""
        img = Image()
        start = TEXT
        body_len = 4 + len(middle) + 4
        cases = [start + 4 * (body_len + 2 * n) for n in range(4)]
        default = start + 4 * (body_len + 8)
        tbl = self.table(img, [default] * lead + cases)
        words = [cmplwi(6, 3, imm), bc(bo, 24 + bit, default - (start + 4)), lis(11, ha(tbl)), addi(11, 11, lo(tbl))]
        words += middle + [slwi(10, 3, 2), lwzx(11, 10, 11), mtctr(11), BCTR]
        assert len(words) == body_len
        for n in range(4):
            words += [li(3, n), BLR]
        words += [li(3, -1), BLR]
        _function(img, words)
        return img, start + 4 * (body_len - 1), cases

    def test_plain_dispatch_is_found(self):
        img, site, cases = self.dispatch([])
        found, *_ = find(img)
        self.assertEqual((found[site]["table"], found[site]["labels"]), (RDATA, cases))

    def test_unmodelled_instruction_forgets_everything(self):
        img, site, _ = self.dispatch([0x44000002])                # sc
        found, unresolved, _, _ = find(img)
        self.assertNotIn(site, found)
        self.assertIn(site, unresolved)

    def test_call_clobbers_the_volatile_table_register(self):
        img, site, _ = self.dispatch([bl(0x100)])
        found, unresolved, _, _ = find(img)
        self.assertNotIn(site, found)
        self.assertIn(site, unresolved)

    def test_update_load_moves_the_table_register(self):
        # lwzu r9,4(r11) leaves r11 = table + 4: the dispatch reads the entries after the first one
        img, site, cases = self.dispatch([lwzu(9, 4, 11)], lead=1)
        found, *_ = find(img)
        self.assertEqual((found[site]["table"], found[site]["labels"]), (RDATA + 4, cases))

    def test_wrong_direction_branch_is_no_bound(self):
        img, site, _ = self.dispatch([], bo=12, bit=0)         # blt: the fall-through has r3 >= 3
        found, unresolved, unbounded, _ = find(img)
        self.assertNotIn(site, found)

    def test_ctr_decrementing_branch_gives_no_bound(self):
        # bdnzt cr6.gt: not taken when CTR hits zero OR r3 > 3, so the fall-through proves nothing about r3
        img, site, _ = self.dispatch([], bo=0b01000, bit=1)
        found, unresolved, unbounded, _ = find(img)
        self.assertNotIn(site, found)
        self.assertIn(site, unbounded)

    def test_tightest_of_two_bounds_is_used(self):
        # The dispatch's own check (r3 <= 3) comes first, a loose one (r3 <= 200) after it: 4 entries, not 201
        img, start = Image(), TEXT
        tbl = img.alloc(4 * 5 + 8)
        default = start + 4 * (10 + 8)
        body = [cmplwi(6, 3, 3), bc(12, 25, default - (start + 4)), cmplwi(7, 3, 200), bc(12, 29, default - (start + 12)),
                lis(11, ha(tbl)), addi(11, 11, lo(tbl)), slwi(10, 3, 2), lwzx(11, 10, 11), mtctr(11), BCTR]
        cases = [start + 4 * (len(body) + 2 * n) for n in range(4)]
        for n, lbl in enumerate(cases + [cases[0]]):      # a fifth word after the table looks like a label too
            img.put(tbl + 4 * n, lbl)
        for n in range(4):
            body += [li(3, n), BLR]
        body += [li(3, -1), BLR]
        _function(img, body)
        found, *_ = find(img)
        t = found[start + 4 * 9]
        self.assertEqual((t["labels"], t["register"]), (cases, 3))

    def test_vtable_call_is_other(self):
        img = Image()
        _function(img, [lwz(11, 0, 3), lwz(11, 8, 11), mtctr(11), BCTR])
        found, unresolved, unbounded, other = find(img)
        self.assertEqual((found, unresolved, unbounded, other), ({}, {}, {}, 1))


class RandomTests(unittest.TestCase):
    """Seeded random shapes: the supported ones are found and every reported table is right."""

    def test_random_dispatches(self):
        rng = random.Random(20261019)
        seen, total = {}, 0
        for _ in range(12):
            img = Image()
            ds = [build(img, rng, join=rng.choice([None] * 6 + ["pre", "post", "bad", "ctr"])) for _ in range(25)]
            found, unresolved, unbounded, _ = find(img)
            for d in ds:
                total += 1
                t = found.get(d.bctr)
                if d.expect_found:
                    self.assertIsNotNone(t, f"{d.__dict__}: {unresolved.get(d.bctr) or unbounded.get(d.bctr)}")
                elif d.join in ("bad", "ctr"):
                    self.assertIsNone(t, d.__dict__)
                if t:
                    check_table(self, img, d, t, rng)
                    key = (d.kind, d.bound, d.scale if d.kind != "byte" else "-", d.load)
                    seen[key] = seen.get(key, 0) + 1
        self.assertEqual(total, 300)
        self.assertGreater(len(seen), 40)


if __name__ == "__main__":
    unittest.main()
