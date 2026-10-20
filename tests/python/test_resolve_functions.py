"""Unit tests for recomp/tools/resolve_functions.py on synthetic images and codegen logs."""
import importlib.util
import json
import os
import struct
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "resolve_functions", os.path.join(ROOT, "recomp", "tools", "resolve_functions.py"))
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

BASE = 0x82000000
TEXT = BASE + 0x1000      # .text: 0x82001000 .. 0x82002000
PDATA_RVA = 0x2000


def b(src, dst, link=False):
    """b / bl from src to dst."""
    return (18 << 26) | ((dst - src) & 0x03FFFFFC) | (1 if link else 0)


def bc(src, dst, bo=12, bi=2):
    return (16 << 26) | (bo << 21) | (bi << 16) | ((dst - src) & 0xFFFC)


ADDI = 0x38630001        # addi r3,r3,1
BLR = 0x4E800020


def make_pe(words, pdata=(), sections=()):
    """A PE32 image: .text at RVA 0x1000 holding `words` {address: word}, .pdata at RVA 0x2000, then one
    0x1000-byte section per (name, executable, {address: word}) of `sections` at RVA 0x3000, 0x4000, ..."""
    img = bytearray(0x3000 + 0x1000 * len(sections))
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, 0x80)
    img[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", img, 0x80 + 6, 2 + len(sections))   # sections
    struct.pack_into("<H", img, 0x80 + 20, 0xE0)      # optional header size
    opt = 0x80 + 24
    struct.pack_into("<H", img, opt, 0x10B)
    struct.pack_into("<II", img, opt + 96 + 3 * 8, PDATA_RVA, len(pdata) * 8)
    sect = opt + 0xE0
    img[sect:sect + 8] = b".text\0\0\0"
    struct.pack_into("<II", img, sect + 8, 0x1000, 0x1000)
    struct.pack_into("<I", img, sect + 36, 0x60000020)
    img[sect + 40:sect + 48] = b".pdata\0\0"
    struct.pack_into("<II", img, sect + 48, 0x1000, PDATA_RVA)
    struct.pack_into("<I", img, sect + 76, 0x40000040)
    for i, (name, executable, content) in enumerate(sections):
        off = sect + 80 + 40 * i
        img[off:off + 8] = name.encode("ascii").ljust(8, b"\0")
        struct.pack_into("<II", img, off + 8, 0x1000, 0x3000 + 0x1000 * i)
        struct.pack_into("<I", img, off + 36, 0x60000020 if executable else 0x40000040)
        for addr, w in content.items():
            struct.pack_into(">I", img, addr - BASE, w)
    for addr, w in words.items():
        struct.pack_into(">I", img, addr - BASE, w)
    for i, (begin, size) in enumerate(pdata):
        struct.pack_into(">II", img, PDATA_RVA + i * 8, begin, ((size // 4) << 8) | (1 << 30))
    return bytes(img)


def image(words, pdata=(), sections=()):
    return MOD.Image(make_pe(words, pdata, sections), base=BASE)


LOG = """ReXGlue v0.10.0.2-dev.gc94f5eb - Xbox 360 Recompilation Toolkit
System.Management.Automation.RemoteException
Found __savegprlr_14 at 0x82BAF650
Found __restgprlr_14 at 0x82BAF6A0
FunctionNode 0x82001100 (sub_82001100): DISCOVERED with 1 blocks, 2 instructions, 0 labels
FunctionGraph::sealAllReady: 0x82001100 (sub_82001100) cannot seal (1 unresolved)
  0x82001104 -> 0x82001010
FunctionGraph::sealAllReady: 0x82001200 (sub_82001200) cannot seal (2 unresolved)
  0x82001208 -> 0x82001A00
  0x8200120C -> 0x82001A10
FunctionGraph::sealAllReady: sealed 10 functions, 2 could not seal
  phase  default.xex: Validate
Analyze: found 4 errors
=== ANALYSIS ERRORS ===
UnresolvedCall (3):
  0x82001010 from 0x82001104: b 0x82001010 from 0x82001104 - target not in any function
  0x82001A00 from 0x82001208: b 0x82001A00 from 0x82001208 - target not in any function
  0x82001A10 from 0x8200120C: bl 0x82001A10 from 0x8200120C in sub_82001200 - no CallEdge recorded
MissingJumpTable (1):
  0x82001300: bctr without a table
Total: 4 errors
Analysis failed for 'default'
"""


class DecodeTests(unittest.TestCase):
    def test_branches(self):
        self.assertEqual(MOD.decode_branch(0x82001104, b(0x82001104, 0x82001010))["target"], 0x82001010)
        fwd = MOD.decode_branch(0x82001000, b(0x82001000, 0x82101000, link=True))
        self.assertEqual((fwd["mnemonic"], fwd["target"], fwd["link"]), ("bl", 0x82101000, True))
        absolute = MOD.decode_branch(0x82001000, (18 << 26) | 0x1000 | 2)
        self.assertEqual((absolute["mnemonic"], absolute["target"]), ("ba", 0x1000))
        cond = MOD.decode_branch(0x82001000, bc(0x82001000, 0x82000FF0))
        self.assertEqual((cond["target"], cond["conditional"]), (0x82000FF0, True))
        self.assertFalse(MOD.decode_branch(0x82001000, bc(0x82001000, 0x82001010, bo=20))["conditional"])
        self.assertIsNone(MOD.decode_branch(0x82001000, ADDI))

    def test_falls_through(self):
        for w in (BLR, 0x4E800420, 0, 0xFFFFFFFF, b(0x82001000, 0x82001100), 0x0FE00016, 0x7FE00008,
                  bc(0x82001000, 0x82001010, bo=20)):
            self.assertFalse(MOD.falls_through(w), hex(w))
        for w in (ADDI, b(0x82001000, 0x82001100, link=True), bc(0x82001000, 0x82001010)):
            self.assertTrue(MOD.falls_through(w), hex(w))


class LogTests(unittest.TestCase):
    def test_parse(self):
        p = MOD.parse_log(LOG)
        self.assertEqual(p["total"], 4)
        self.assertEqual(p["helpers"], {"savegprlr_14": 0x82BAF650, "restgprlr_14": 0x82BAF6A0})
        self.assertEqual([(u["target"], u["site"], u["mnemonic"]) for u in p["unresolved"]],
                         [(0x82001010, 0x82001104, "b"), (0x82001A00, 0x82001208, "b"),
                          (0x82001A10, 0x8200120C, "bl")])
        self.assertEqual(p["unresolved"][0]["function"], (0x82001100, "sub_82001100"))
        self.assertEqual(p["unresolved"][2]["function"], (0x82001200, "sub_82001200"))
        self.assertEqual(len(p["errors"]["MissingJumpTable"]), 1)

    def test_success_log_has_nothing(self):
        p = MOD.parse_log("Codegen summary: 9 written\nDone in 0.1s.\n")
        self.assertEqual((p["unresolved"], p["errors"]), ([], {}))


FINGERPRINT = "".join(f"#   {n}: 0x{a:08X}\n" for n, a in zip(
    MOD.HELPERS, (0x82BAF650, 0x82BAF6A0, 0x82BB02F0, 0x82BB033C, 0x82BB12C0, 0x82BB1558, 0x82BB1354, 0x82BB15EC)))
LOG_HELPERS = {"savegprlr_14": 0x82BAF650, "restgprlr_14": 0x82BAF6A0, "savefpr_14": 0x82BB02F0,
               "restfpr_14": 0x82BB033C, "savevmx_14": 0x82BB12C0, "restvmx_14": 0x82BB1558,
               "savevmx_64": 0x82BB1354, "restvmx_64": 0x82BB15EC}


class ReferenceTests(unittest.TestCase):
    def write(self, text):
        f = tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False)
        f.write(text)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_fingerprint_rules(self):
        path = self.write("# header\n" + FINGERPRINT + '[functions]\n"0x82001010" = { size = 0x8 }\n"0x82001020" = { end = 0x82001030 }\n')
        funcs, st = MOD.load_reference(path, LOG_HELPERS)
        self.assertTrue(st["applies"], st)
        self.assertEqual(funcs, {0x82001010: 8, 0x82001020: 0x10})
        other = dict(LOG_HELPERS, restvmx_64=0x82BB15F0)
        _, st = MOD.load_reference(path, other)
        self.assertFalse(st["applies"])
        self.assertIn("7 of 8", st["detail"])
        _, st = MOD.load_reference(path, {})
        self.assertFalse(st["applies"])
        _, st = MOD.load_reference(self.write('[functions]\n"0x82001010" = { size = 0x8 }\n'), {})
        self.assertTrue(st["applies"])
        self.assertIn("no helper fingerprint", st["detail"])

    def test_fingerprint_of_the_converted_ttt2web_table(self):
        funcs, st = MOD.load_reference(os.path.join(ROOT, "recomp", "ttt2_functions.ttt2web.toml"), LOG_HELPERS)
        self.assertTrue(st["applies"], st)
        self.assertEqual(len(funcs), 18351)
        self.assertEqual(funcs[0x826CC0A8], 0x30)
        self.assertEqual(funcs[0x82959A80], 0x78)


def ref_of(entries, pdata=()):
    r = MOD.Reference()
    for a, size in entries.items():
        r.add(a, size, "reference table t.toml")
    for a, size in pdata:
        r.add(a, size, ".pdata")
    r.finish()
    return r


class DecideTests(unittest.TestCase):
    # .text layout: thunk at 0x82001100 (addi; b leaf), leaf at 0x82001010, a function F at
    # 0x82001400 whose out-of-line block at 0x82001440 branches back to 0x82001420.
    WORDS = {0x82001010: ADDI, 0x82001014: BLR, 0x8200100C: BLR,
             0x82001100: ADDI, 0x82001104: b(0x82001104, 0x82001010),
             0x82001400: ADDI, 0x82001420: ADDI, 0x8200143C: BLR, 0x82001440: ADDI,
             0x82001444: b(0x82001444, 0x82001420), 0x82001500: b(0x82001500, 0x82001800)}

    def err(self, target, site, mnemonic="b"):
        return {"target": target, "site": site, "mnemonic": mnemonic, "function": None}

    def test_entry(self):
        d = MOD.decide(self.err(0x82001010, 0x82001104), image(self.WORDS), ref_of({0x82001010: 8}))
        self.assertEqual((d["decision"], d["entry"], d["size"]), ("entry", 0x82001010, 0))
        self.assertFalse(d["target_preceded_by_fallthrough"])
        self.assertIn("lists a function starting at 0x82001010", d["evidence"])

    def test_entry_from_pdata(self):
        d = MOD.decide(self.err(0x82001010, 0x82001104), image(self.WORDS), ref_of({}, [(0x82001010, 8)]))
        self.assertEqual(d["decision"], "entry")
        self.assertIn(".pdata", d["evidence"])

    def test_function(self):
        d = MOD.decide(self.err(0x82001420, 0x82001444), image(self.WORDS), ref_of({0x82001400: 0x48}))
        self.assertEqual((d["decision"], d["entry"], d["size"]), ("function", 0x82001400, 0x48))

    def test_unverified_cases(self):
        img = image(self.WORDS)
        ref = ref_of({0x82001400: 0x48, 0x82001010: 8}, [(0x82001600, 0x40)])
        cases = [
            (self.err(0x82001010, 0x82001100), "not a direct branch"),          # log/image mismatch
            (self.err(0x82001800, 0x82001500), "is not code"),                  # zero word at the target
            (self.err(0x82001420, 0x82002400), "not in the image's code"),      # site outside .text
            (self.err(0x82001010, 0x82001104), "no boundary reference"),        # no reference at all
        ]
        for i, (e, why) in enumerate(cases):
            d = MOD.decide(e, img, None if i == 3 else ref)
            self.assertEqual(d["decision"], "unverified", why)
            self.assertIn(why, d["evidence"])
        # Branch from outside a reference function into its middle: not an entry, not internal.
        words = dict(self.WORDS)
        words[0x82001104] = b(0x82001104, 0x82001420)
        d = MOD.decide(self.err(0x82001420, 0x82001104), image(words), ref)
        self.assertEqual(d["decision"], "unverified")
        self.assertIn("branch comes from outside it", d["evidence"])
        # Inside a .pdata function: ReXGlue should have contained it; never guessed.
        words[0x82001104] = b(0x82001104, 0x82001610)
        words[0x82001610] = ADDI
        d = MOD.decide(self.err(0x82001610, 0x82001104), image(words), ref)
        self.assertIn("inside the .pdata function", d["evidence"])


class OutputTests(unittest.TestCase):
    def test_round_trip_overlap_and_image_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "resolved.toml")
            entries = {}
            decisions = [
                {"decision": "function", "entry": 0x82001400, "size": 0x48, "instruction": "b 0x82001420",
                 "site": 0x82001444, "function": (0x82001440, "sub_82001440"), "evidence": "e"},
                {"decision": "entry", "entry": 0x82001010, "size": 0, "instruction": "b 0x82001010",
                 "site": 0x82001104, "function": None, "evidence": "e"},
                {"decision": "entry", "entry": 0x82001420, "size": 0, "instruction": "b 0x82001420",
                 "site": 0x82001104, "function": None, "evidence": "e"},   # inside the sized entry
            ]
            added, conflicts = MOD.apply_decisions(entries, decisions)
            self.assertEqual(added, [0x82001400, 0x82001010])
            self.assertEqual(len(conflicts), 1)
            self.assertEqual(decisions[2]["decision"], "unverified")
            MOD.write_out(out, entries, "a" * 40)
            text = open(out, encoding="utf-8").read()
            self.assertIn('"0x82001010" = {}  # entry: b 0x82001010 at 0x82001104', text)
            self.assertIn('"0x82001400" = { size = 0x48 }  # function: b 0x82001420 at 0x82001444 (in sub_82001440)', text)
            self.assertIn("image-sha1: " + "a" * 40, text)
            back, sha1 = MOD.read_out(out)
            self.assertEqual(sha1, "a" * 40)
            self.assertEqual({a: s for a, (s, _) in back.items()}, {0x82001010: 0, 0x82001400: 0x48})
            img = image(DecideTests.WORDS)
            state = {}
            self.assertEqual(MOD.start_entries(out, img, state), {})     # another image's entries
            self.assertIn("were discarded", state["discarded"])
            MOD.write_out(out, back, img.sha1)
            self.assertEqual(len(MOD.start_entries(out, img, {})), 2)    # the same image keeps them
            # A conflicting size for an existing entry is refused.
            again = [{"decision": "function", "entry": 0x82001400, "size": 0x50, "instruction": "b x",
                      "site": 0x82001444, "function": None, "evidence": "e"}]
            added, conflicts = MOD.apply_decisions(back, again)
            self.assertEqual((added, len(conflicts)), ([], 1))


class BootTests(unittest.TestCase):
    """The boot-log mode: addresses the game called through a pointer that no function starts at."""
    LINE = "[2026-10-19 20:03:48.251] [critical] [cpu] [t5] [FATAL] Call to invalid or unregistered function at guest address {}\n"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name, text):
        p = os.path.join(self.tmp.name, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def test_parse_boot_logs(self):
        a = self.path("a.log", "noise\n" + self.LINE.format("0x82001010") + self.LINE.format("0x82001010"))
        b = self.path("b.log", self.LINE.format("0x82001400"))
        found = MOD.parse_boot_logs([a, b])
        self.assertEqual(sorted(found), [0x82001010, 0x82001400])
        self.assertEqual(found[0x82001010], (a, 2))

    def test_decide_runtime(self):
        img = image(DecideTests.WORDS)
        ref = ref_of({0x82001010: 8, 0x82001400: 0x48})
        d = MOD.decide_runtime(0x82001010, ("x.log", 3), img, ref)
        self.assertEqual((d["decision"], d["entry"], d["size"]), ("entry", 0x82001010, 0))
        self.assertIn("through a pointer (x.log line 3)", d["evidence"])
        d = MOD.decide_runtime(0x82001420, ("x.log", 3), img, ref)
        self.assertEqual(d["decision"], "unverified")
        self.assertIn("middle of a function", d["evidence"])
        self.assertIn("not code", MOD.decide_runtime(0x82001800, ("x.log", 3), img, ref)["evidence"])

    def test_unread_switch_dispatch_named_for_a_jump_into_its_function(self):
        img = image(DecideTests.WORDS)
        ref = ref_of({0x82001010: 8, 0x82001400: 0x48})
        sw = self.path("switch_tables.json", json.dumps({"unresolved_sites": [
            {"bctr": "0x82001410", "kind": "bounded", "reason": "x"}, {"bctr": "0x82001600", "kind": "bounded", "reason": "y"}]}))
        ds = [MOD.decide_runtime(0x82001420, ("x.log", 3), img, ref), MOD.decide_runtime(0x82001010, ("x.log", 4), img, ref)]
        MOD.note_unread_dispatches(ds, sw, img, ref)
        self.assertEqual(ds[0]["unread_dispatches"], [0x82001410])
        self.assertIn("holds the switch dispatch 0x82001410 that the switch-table finder could not read", ds[0]["evidence"])
        self.assertNotIn("unread_dispatches", ds[1])
        MOD.note_unread_dispatches(ds[:1], self.path("bad.json", "{"), img, ref)
        self.assertIn("could not be read", ds[0]["evidence"])
        self.assertIn("no boundary reference", MOD.decide_runtime(0x82001010, ("x.log", 3), img, None)["evidence"])

    def args(self, logs, out, reference, codegen_log=None):
        ns = type("A", (), {})()
        for k, v in dict(image=self.xex, base="0x82000000", boot_log=logs, codegen_log=codegen_log, reference=reference,
                         out=out, report=os.path.join(self.tmp.name, "boot.md"),
                         json=os.path.join(self.tmp.name, "boot.json")).items():
            setattr(ns, k, v)
        return ns

    def test_cmd_boot(self):
        self.xex = os.path.join(self.tmp.name, "default.pe")
        with open(self.xex, "wb") as f:
            f.write(make_pe(DecideTests.WORDS))
        ref = self.path("ref.toml", '[functions]\n"0x82001010" = { size = 0x8 }\n"0x82001400" = { size = 0x48 }\n')
        out = os.path.join(self.tmp.name, "resolved.toml")
        log = self.path("ttt2_001.log", self.LINE.format("0x82001010"))
        self.assertEqual(MOD.cmd_boot(self.args([log], out, ref)), 0)
        entries, _ = MOD.read_out(out)
        self.assertEqual(entries[0x82001010][0], 0)
        self.assertIn("called through a pointer at run time (ttt2_001.log line 1)", entries[0x82001010][1])
        import json
        self.assertEqual(json.load(open(os.path.join(self.tmp.name, "boot.json")))["added"], [0x82001010])
        # The same trap again: nothing new, and the report says the host still lacked the entry.
        self.assertEqual(MOD.cmd_boot(self.args([log], out, ref)), 0)
        self.assertEqual(json.load(open(os.path.join(self.tmp.name, "boot.json")))["added"], [])
        self.assertIn("already present", open(os.path.join(self.tmp.name, "boot.md")).read())
        # A call into a function's middle is never turned into an entry.
        mid = self.path("ttt2_002.log", self.LINE.format("0x82001420"))
        self.assertEqual(MOD.cmd_boot(self.args([mid], out, ref)), 3)
        # A fingerprinted reference needs the codegen log's helper lines to apply.
        fp_ref = self.path("fp.toml", "# h\n" + FINGERPRINT + '[functions]\n"0x82001010" = { size = 0x8 }\n')
        out2 = os.path.join(self.tmp.name, "resolved2.toml")
        self.assertEqual(MOD.cmd_boot(self.args([log], out2, fp_ref)), 3)
        cg = self.path("codegen.log", "".join(f"Found __{n} at 0x{a:08X}\n" for n, a in LOG_HELPERS.items()))
        self.assertEqual(MOD.cmd_boot(self.args([log], out2, fp_ref, cg)), 0)
        # No trap in the logs at all.
        quiet = self.path("ttt2_003.log", "nothing to see\n")
        self.assertEqual(MOD.cmd_boot(self.args([quiet], out, ref)), 0)
        self.assertIn("no calls to unregistered functions", open(os.path.join(self.tmp.name, "boot.md")).read())


class LoopTests(unittest.TestCase):
    """cmd_codegen with run_rexglue replaced: round logic without an SDK."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        words = dict(DecideTests.WORDS)
        self.xex = os.path.join(d, "default.pe")
        with open(self.xex, "wb") as f:
            f.write(make_pe(words))
        self.ref = os.path.join(d, "ref.toml")
        with open(self.ref, "w") as f:
            f.write('[functions]\n"0x82001010" = { size = 0x8 }\n"0x82001400" = { size = 0x48 }\n')
        self.manifest = os.path.join(d, "m.toml")
        open(self.manifest, "w").close()
        self.fake = os.path.join(d, "rexglue")
        open(self.fake, "w").close()
        self.out = os.path.join(d, "resolved.toml")
        self.log = os.path.join(d, "codegen.log")
        self.saved = MOD.run_rexglue
        self.addCleanup(setattr, MOD, "run_rexglue", self.saved)

    def args(self, **kw):
        ns = type("A", (), {})()
        for k, v in dict(rexglue=self.fake, manifest=self.manifest, image=self.xex, base="0x82000000",
                         out=self.out, reference=self.ref, log=self.log, report=os.path.join(self.tmp.name, "r.md"),
                         json=os.path.join(self.tmp.name, "r.json"), rounds=4, ignore_stamp=False,
                         log_level="debug").items():
            setattr(ns, k, v)
        for k, v in kw.items():
            setattr(ns, k, v)
        return ns

    def fake_rexglue(self, script):
        calls = []

        def run(args, round_no, log_path):
            text = open(self.out, encoding="utf-8").read()
            body = script(round_no, text)
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(body)
            calls.append(round_no)
            return (0 if "Codegen summary" in body else 1), 0.0
        MOD.run_rexglue = run
        return calls

    ERR = ("=== ANALYSIS ERRORS ===\nUnresolvedCall (2):\n"
           "  0x82001010 from 0x82001104: b 0x82001010 from 0x82001104 - target not in any function\n"
           "  0x82001420 from 0x82001444: b 0x82001420 from 0x82001444 - target not in any function\n"
           "Total: 2 errors\n")

    def test_resolves_then_succeeds(self):
        calls = self.fake_rexglue(lambda n, text: "Codegen summary: ok\n" if '"0x82001400"' in text else self.ERR)
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1, 2])
        self.assertIn("codegen succeeded in round 2 with 2 resolved entries", open(self.args().report).read())
        self.assertTrue(os.path.exists(os.path.splitext(self.log)[0] + ".round1.log"))

    def test_no_progress_stops(self):
        calls = self.fake_rexglue(lambda n, text: self.ERR)
        self.assertEqual(MOD.cmd_codegen(self.args()), 3)
        self.assertEqual(calls, [1, 2])
        self.assertIn("no new entries", open(self.args().report).read())

    def test_unverified_stops(self):
        calls = self.fake_rexglue(lambda n, text: self.ERR)
        self.assertEqual(MOD.cmd_codegen(self.args(reference=None)), 3)
        self.assertEqual(calls, [1])
        self.assertIn("could not be verified", open(self.args().report).read())
        self.assertTrue(os.path.exists(self.out))       # the include exists even then

    def test_other_failure_stops(self):
        calls = self.fake_rexglue(lambda n, text: "Error: something else\n")
        self.assertEqual(MOD.cmd_codegen(self.args()), 1)
        self.assertEqual(calls, [1])

    def test_success_first_time_writes_empty_include(self):
        self.fake_rexglue(lambda n, text: "Codegen summary: ok\n")
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        entries, sha1 = MOD.read_out(self.out)
        self.assertEqual(entries, {})
        self.assertIsNotNone(sha1)



def leaf(words, addr):
    words[addr] = ADDI
    words[addr + 4] = BLR


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class IndirectTests(unittest.TestCase):
    """The ahead-of-time rules: which reference functions ReXGlue did not register get an entry."""
    A, T1, T2, F, T3 = 0x82001000, 0x82001008, 0x82001010, 0x82001020, 0x82001028
    P, T4 = 0x82001100, 0x82001110
    T5, T6, T7, T8, T9 = 0x82001200, 0x82001300, 0x82001400, 0x82001500, 0x82001600
    T10, T11, T12, T13, T14, T15 = 0x82001700, 0x82001800, 0x82001900, 0x82001A00, 0x82001B00, 0x82001FF8
    RDATA, DATA, RELOC, TEXT2 = 0x82003000, 0x82004000, 0x82005000, 0x82006000

    def build(self):
        w = {}
        for a in (self.A, self.T1, self.T2, self.T3, self.T5, self.T9, self.T11, self.T12, self.T13, self.T14,
                  self.T15):
            leaf(w, a)
        w[self.F], w[self.F + 4] = ADDI, b(self.F + 4, self.A, link=True)      # bl: falls through into T3
        for i, x in enumerate((ADDI, ADDI, ADDI, BLR, ADDI, BLR, ADDI, BLR)):  # .pdata function P, T4 inside
            w[self.P + 4 * i] = x
        w[self.T6], w[self.T6 + 4], w[self.T6 + 8], w[self.T6 + 12] = ADDI, BLR, ADDI, BLR   # ReXGlue: T6+8
        w[self.T7], w[self.T7 + 4] = ADDI, ADDI                                 # ends without a branch
        w[self.T8] = ADDI                                                       # T8+4 stays zero
        rdata = {self.RDATA: self.T1, self.RDATA + 0x10: self.T1, self.RDATA + 4: self.T3, self.RDATA + 8: self.T4,
                 self.RDATA + 0xC: self.T5, self.RDATA + 0x14: self.T6, self.RDATA + 0x18: self.T7,
                 self.RDATA + 0x1C: self.T8, self.RDATA + 0x20: self.T9, self.RDATA + 0x24: self.T10,
                 self.RDATA + 0x28: self.T13, self.RDATA + 0x2C: self.T14, self.RDATA + 0x30: self.T15,
                 # T2's bytes at an unaligned offset only: 00 00 82 00 | 10 10 FF FF
                 self.RDATA + 0x40: 0x00008200, self.RDATA + 0x44: 0x1010FFFF}
        sections = [(".rdata", False, rdata), (".data", False, {self.DATA: self.T12}),
                    (".reloc", False, {self.RELOC: self.T11}),
                    (".text2", True, {self.TEXT2: self.T11, self.TEXT2 + 4: self.T2})]
        img = image(w, pdata=[(self.A, 8), (self.P, 0x20)], sections=sections)
        sizes = {t: 8 for t in (self.T1, self.T2, self.T3, self.T4, self.T5, self.T7, self.T8, self.T9, self.T10,
                                self.T11, self.T12, self.T13, self.T14)}
        sizes.update({self.T6: 0x10, self.T15: 0x10})
        return img, ref_of(sizes, pdata=[(self.A, 8), (self.P, 0x20)])

    def test_rules(self):
        img, ref = self.build()
        registered = {self.A, self.P, self.T6 + 8, self.T14}
        entries = {self.T13: (0, "entry: b 0x82001A00 at 0x82001A40")}
        ds = MOD.decide_indirect(img, ref, registered, {self.T5: 0x82001F00}, entries,
                                 {self.T9: "codegen failed in round 2 (MissingJumpTable 1) with it registered ahead of time"})
        got = {d["target"]: d for d in ds}
        self.assertNotIn(self.T13, got)                 # already an entry
        self.assertNotIn(self.T14, got)                 # already registered
        expect = {self.T1: "registered", self.T2: "no-pointer", self.T3: "fallthrough", self.T4: "pdata",
                  self.T5: "switch-label", self.T6: "overlap", self.T7: "extent", self.T8: "extent",
                  self.T9: "excluded", self.T10: "not-code", self.T11: "no-pointer", self.T12: "registered",
                  self.T15: "extent"}
        self.assertEqual({t: d["reason"] for t, d in got.items()}, expect)
        self.assertEqual((got[self.T1]["decision"], got[self.T1]["entry"], got[self.T1]["size"]), ("entry", self.T1, 0))
        self.assertEqual(got[self.T1]["origin"], "pointer at 0x82003000 (.rdata) and 1 more")
        self.assertEqual(got[self.T1]["pointers"], [self.RDATA, self.RDATA + 0x10])
        self.assertEqual(got[self.T12]["origin"], "pointer at 0x82004000 (.data)")
        self.assertIn("ends at 0x82001404 with an instruction that falls through", got[self.T7]["evidence"])
        self.assertIn("holds a zero word at 0x82001504", got[self.T8]["evidence"])
        self.assertIn("leaves the code section", got[self.T15]["evidence"])
        self.assertIn("inside the .pdata function 0x82001100+0x20", got[self.T4]["evidence"])
        self.assertIn("case label of the switch at 0x82001F00", got[self.T5]["evidence"])
        self.assertIn("ReXGlue registered 0x82001308 inside its reference extent", got[self.T6]["evidence"])
        self.assertIn("(0x82001024: 0x", got[self.T3]["evidence"])
        # Applied, the two entries carry the ahead-of-time mark the pipeline and --drop-indirect rely on.
        added, conflicts = MOD.apply_decisions(entries, ds)
        self.assertEqual((sorted(added), conflicts), ([self.T1, self.T12], []))
        self.assertEqual(entries[self.T12][1], "entry: ahead of time, pointer at 0x82004000 (.data)")

    def test_pointer_sites_skip_code_pdata_and_reloc(self):
        img, _ = self.build()
        found = MOD.pointer_sites(img, [self.T1, self.T2, self.T11, self.T12, self.A])
        self.assertEqual(found, {self.T1: [(self.RDATA, ".rdata"), (self.RDATA + 0x10, ".rdata")],
                                 self.T12: [(self.DATA, ".data")]})
        self.assertEqual(MOD.pointer_sites(img, []), {})

    def test_generated_code_and_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = os.path.join(tmp, "m.toml")
            with open(manifest, "w") as f:
                f.write('[entrypoint]\nfile_path = "x.xex"\nout_directory_path = "gen/v"\n')
            gen = MOD.manifest_out_dir(manifest)
            self.assertEqual(gen, os.path.join(tmp, "gen", "v"))
            self.assertIsNone(MOD.registered_functions(gen))
            os.makedirs(gen)
            with open(os.path.join(gen, "x_register.cpp"), "w") as f:
                f.write("  registrar->SetFunction(0x82010000, xstart);\n  registrar->SetFunction(0x820100A8, sub_820100A8);\n")
            self.assertEqual(MOD.registered_functions(gen), {0x82010000, 0x820100A8})
            sw = os.path.join(tmp, "sw.json")
            with open(sw, "w") as f:
                json.dump({"tables": [{"bctr": "0x82001F00", "labels": ["0x82001F10", "0x82001F20", "0x82001F10"]}]}, f)
            self.assertEqual(MOD.switch_labels(sw), {0x82001F10: 0x82001F00, 0x82001F20: 0x82001F00})

    def test_exclusions_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "resolved.toml")
            img = image({0x82001000: BLR})
            MOD.write_out(out, {0x82001008: (0, "entry: ahead of time, pointer at 0x82003000 (.rdata)")}, img.sha1,
                          {0x82001010: "codegen failed in round 2\n(MissingJumpTable 1)"})
            text = read(out)
            self.assertIn("# excluded 0x82001010: codegen failed in round 2 (MissingJumpTable 1)", text)
            entries, sha1 = MOD.read_out(out)
            self.assertEqual(list(entries), [0x82001008])        # the exclusion is no entry
            self.assertEqual(MOD.start_exclusions(out, img), {0x82001010: "codegen failed in round 2 (MissingJumpTable 1)"})
            self.assertEqual(MOD.start_exclusions(out, image({0x82001000: ADDI})), {})   # another image's


class IndirectLoopTests(unittest.TestCase):
    """cmd_codegen --indirect with run_rexglue replaced: the batch, its exclusion and its rollback."""
    A, U1, U2, THUNK, L, THUNK2, L2 = 0x82001000, 0x82001008, 0x82001010, 0x82001100, 0x82001200, 0x82001110, 0x82001210

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        w = {}
        for a in (self.A, self.U1, self.U2, self.L, self.L2):
            leaf(w, a)
        w[self.THUNK], w[self.THUNK + 4] = ADDI, b(self.THUNK + 4, self.L)
        w[self.THUNK2], w[self.THUNK2 + 4] = ADDI, b(self.THUNK2 + 4, self.L2)
        sections = [(".rdata", False, {0x82003000: self.U1}), (".data", False, {0x82004000: self.U2})]
        self.xex = os.path.join(d, "default.pe")
        with open(self.xex, "wb") as f:
            f.write(make_pe(w, sections=sections))
        self.ref = os.path.join(d, "ref.toml")
        with open(self.ref, "w") as f:
            f.write("[functions]\n" + "".join(f'"0x{a:08X}" = {{ size = 0x8 }}\n' for a in (self.U1, self.U2, self.L, self.L2)))
        self.manifest = os.path.join(d, "m.toml")
        with open(self.manifest, "w") as f:
            f.write('[entrypoint]\nfile_path = "default.pe"\nout_directory_path = "generated/default"\n')
        self.switch = os.path.join(d, "switch_tables.json")
        with open(self.switch, "w") as f:
            json.dump({"tables": [], "unresolved_sites": []}, f)
        self.fake = os.path.join(d, "rexglue")
        open(self.fake, "w").close()
        self.out = os.path.join(d, "resolved.toml")
        self.log = os.path.join(d, "codegen.log")
        self.gen = os.path.join(d, "generated", "default")
        self.saved = MOD.run_rexglue
        self.addCleanup(setattr, MOD, "run_rexglue", self.saved)

    def args(self, **kw):
        ns = type("A", (), {})()
        for k, v in dict(rexglue=self.fake, manifest=self.manifest, image=self.xex, base="0x82000000", out=self.out,
                         reference=self.ref, log=self.log, report=os.path.join(self.tmp.name, "r.md"),
                         json=os.path.join(self.tmp.name, "r.json"), rounds=6, ignore_stamp=False, log_level="debug",
                         indirect=True, switch_json=self.switch, drop_indirect=False).items():
            setattr(ns, k, v)
        for k, v in kw.items():
            setattr(ns, k, v)
        return ns

    @staticmethod
    def error_log(category, lines):
        return (f"=== ANALYSIS ERRORS ===\n{category} ({len(lines)}):\n" + "".join(f"  {x}\n" for x in lines)
                + f"Total: {len(lines)} errors\n")

    def fake_rexglue(self, script, register=True):
        """`script(round, entries)` returns None for success or the log of a failed round."""
        calls = []

        def run(args, round_no, log_path):
            entries, _ = MOD.read_out(self.out)
            failure = script(round_no, entries)
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(failure if failure is not None else "Codegen summary: ok\n")
            if failure is None and register:
                os.makedirs(self.gen, exist_ok=True)
                with open(os.path.join(self.gen, "t_register.cpp"), "w") as f:
                    for a in sorted({self.A, self.THUNK, self.THUNK2} | set(entries)):
                        f.write(f"  registrar->SetFunction(0x{a:08X}, sub_{a:08X});\n")
            calls.append(round_no)
            return (0 if failure is None else 1), 0.0
        MOD.run_rexglue = run
        return calls

    def report(self):
        return read(os.path.join(self.tmp.name, "r.md"))

    def test_batch_accepted_after_the_error_rounds(self):
        unresolved = f"0x{self.L:08X} from 0x{self.THUNK + 4:08X}: b 0x{self.L:08X} from 0x{self.THUNK + 4:08X} - target not in any function"
        calls = self.fake_rexglue(lambda n, e: None if self.L in e else self.error_log("UnresolvedCall", [unresolved]))
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1, 2, 3])
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(sorted(entries), [self.U1, self.U2, self.L])
        self.assertTrue(entries[self.U1][1].startswith("entry: ahead of time, pointer at 0x82003000 (.rdata)"))
        md = self.report()
        self.assertIn("codegen succeeded in round 3 with 3 resolved entries (2 of them ahead of time)", md)
        self.assertIn("Round 2: rexglue exit 0, 0 unresolved calls; then 2 functions registered ahead of time", md)
        self.assertIn("2 functions registered ahead of time; codegen accepted them in round 3", md)
        self.assertIn("Every entry of the output file is registered in the generated code", md)
        self.assertEqual(json.loads(read(os.path.join(self.tmp.name, "r.json")))["indirect"]["status"], "accepted")
        # A second run finds nothing new and succeeds in its first round.
        calls[:] = []
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1])
        self.assertIn("nothing new to register ahead of time", self.report())

    def test_errors_inside_one_batch_function_take_it_out(self):
        bad = self.error_log("MissingJumpTable", [f"0x{self.U1 + 4:08X}: bctr without a table"])
        calls = self.fake_rexglue(lambda n, e: bad if self.U1 in e else None)
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1, 2, 3])
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(sorted(entries), [self.U2])
        self.assertEqual(MOD.read_exclusions(self.out),
                         {self.U1: "codegen failed in round 2 (MissingJumpTable 1) with it registered ahead of time"})
        md = self.report()
        self.assertIn("Taken out again after codegen failed with them: 1: 0x82001008", md)
        self.assertIn("1 function registered ahead of time; codegen accepted it in round 3", md)
        # The next run skips the excluded function instead of failing with it again.
        calls[:] = []
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1])
        self.assertIn("| taken out in an earlier run (codegen failed with it) | 1 |", self.report())

    def test_failure_nothing_points_into_rolls_the_batch_back(self):
        bad = self.error_log("DiscontinuousFunction", ["0x82001F00: blocks of sub_82001F00 are not contiguous"])
        calls = self.fake_rexglue(lambda n, e: bad if self.U1 in e or self.U2 in e else None)
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1, 2, 3])
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(entries, {})
        self.assertEqual(sorted(MOD.read_exclusions(self.out)), [self.U1, self.U2])
        md = self.report()
        self.assertIn("rolled back: codegen failed in round 2 (DiscontinuousFunction 1); every entry from before the "
                      "batch is kept", md)
        self.assertIn("codegen succeeded in round 3", md)

    def test_rollback_keeps_the_entries_from_before_the_batch(self):
        # An entry from an earlier run (a boot trap, say) stays through a rollback; what came after the batch goes.
        MOD.write_out(self.out, {self.L2: (0, "entry: called through a pointer at run time (x.log line 1)")},
                      MOD.load_image(self.xex, "0x82000000").sha1)
        unresolved = f"0x{self.L:08X} from 0x{self.THUNK + 4:08X}: b 0x{self.L:08X} from 0x{self.THUNK + 4:08X} - target not in any function"
        bad = self.error_log("DiscontinuousFunction", ["0x82001F00: blocks are not contiguous"])

        def script(n, e):
            if self.U1 not in e:
                return None                                  # before the batch, and after the rollback
            return self.error_log("UnresolvedCall", [unresolved]) if self.L not in e else bad
        calls = self.fake_rexglue(script)
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertEqual(calls, [1, 2, 3, 4])
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(sorted(entries), [self.L2])

    def test_rounds_running_out_with_the_batch_roll_it_back(self):
        def err(target, site):
            return self.error_log("UnresolvedCall", [f"0x{target:08X} from 0x{site:08X}: b 0x{target:08X} from 0x{site:08X} - target not in any function"])

        def script(n, e):
            if self.U1 not in e:
                return None
            return err(self.L, self.THUNK + 4) if self.L not in e else err(self.L2, self.THUNK2 + 4)
        calls = self.fake_rexglue(script)
        self.assertEqual(MOD.cmd_codegen(self.args(rounds=3)), 0)
        self.assertEqual(calls, [1, 2, 3, 4])                # one round past the limit, to restore the state
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(entries, {})
        self.assertIn("rolled back: unresolved calls still came back after 3 rounds", self.report())

    def test_drop_indirect_keeps_every_other_entry(self):
        sha1 = MOD.load_image(self.xex, "0x82000000").sha1
        MOD.write_out(self.out, {self.U1: (0, "entry: ahead of time, pointer at 0x82003000 (.rdata)"),
                                 self.L: (0, "entry: b 0x82001200 at 0x82001104")}, sha1, {self.U2: "rolled back"})
        self.fake_rexglue(lambda n, e: None)
        self.assertEqual(MOD.cmd_codegen(self.args(indirect=False, drop_indirect=True)), 0)
        entries, _ = MOD.read_out(self.out)
        self.assertEqual(sorted(entries), [self.L])
        self.assertEqual(MOD.read_exclusions(self.out), {})
        self.assertIn("--drop-indirect removed 1 entries registered ahead of time", self.report())

    def test_unregistered_entry_and_missing_register_file(self):
        sha1 = MOD.load_image(self.xex, "0x82000000").sha1
        MOD.write_out(self.out, {self.L: (0, "entry: b 0x82001200 at 0x82001104")}, sha1)
        os.makedirs(self.gen)
        with open(os.path.join(self.gen, "t_register.cpp"), "w") as f:
            f.write(f"  registrar->SetFunction(0x{self.A:08X}, sub_{self.A:08X});\n")
        self.fake_rexglue(lambda n, e: None, register=False)
        self.assertEqual(MOD.cmd_codegen(self.args(indirect=False)), 0)
        self.assertIn("PROBLEM: 1 entries of the output file are not registered in the generated code (ReXGlue ignored "
                      "them): 0x82001200", self.report())
        os.remove(os.path.join(self.gen, "t_register.cpp"))
        self.assertEqual(MOD.cmd_codegen(self.args()), 0)
        self.assertIn("skipped: the generated code in", self.report())

    def test_indirect_needs_the_switch_report(self):
        argv = ["resolve_functions.py", "codegen", "--rexglue", self.fake, "--manifest", self.manifest, "--image",
                self.xex, "--base", "0x82000000", "--out", self.out, "--indirect"]
        with self.assertRaises(SystemExit) as cm:
            import contextlib, io
            with contextlib.redirect_stderr(io.StringIO()):
                MOD.main(argv)
        self.assertEqual(cm.exception.code, 2)


class BootSummaryTests(unittest.TestCase):
    def test_summary_lines_for_scripts(self):
        import contextlib, io
        with tempfile.TemporaryDirectory() as tmp:
            xex = os.path.join(tmp, "default.pe")
            with open(xex, "wb") as f:
                f.write(make_pe(DecideTests.WORDS))
            ref = os.path.join(tmp, "ref.toml")
            with open(ref, "w") as f:
                f.write('[functions]\n"0x82001010" = { size = 0x8 }\n')
            log = os.path.join(tmp, "ttt2_001.log")
            with open(log, "w") as f:
                f.write(BootTests.LINE.format("0x82001010"))
            ns = type("A", (), {})()
            for k, v in dict(image=xex, base="0x82000000", boot_log=[log], codegen_log=None, reference=ref,
                             out=os.path.join(tmp, "resolved.toml"), report=None, json=None).items():
                setattr(ns, k, v)
            for expect in ("resolve_functions: boot summary: 1 called, 1 added, 0 already present, 0 unverified\n"
                           "resolve_functions: added 0x82001010",
                           "resolve_functions: boot summary: 1 called, 0 added, 1 already present, 0 unverified\n"
                           "resolve_functions: already present 0x82001010"):
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    self.assertEqual(MOD.cmd_boot(ns), 0)
                self.assertIn(expect, buf.getvalue())


if __name__ == "__main__":
    unittest.main()
