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


def make_pe(words, pdata=()):
    """A PE32 image: .text at RVA 0x1000 holding `words` {address: word}, .pdata at RVA 0x2000."""
    img = bytearray(0x3000)
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, 0x80)
    img[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", img, 0x80 + 6, 2)          # sections
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
    for addr, w in words.items():
        struct.pack_into(">I", img, addr - BASE, w)
    for i, (begin, size) in enumerate(pdata):
        struct.pack_into(">II", img, PDATA_RVA + i * 8, begin, ((size // 4) << 8) | (1 << 30))
    return bytes(img)


def image(words, pdata=()):
    return MOD.Image(make_pe(words, pdata), base=BASE)


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


if __name__ == "__main__":
    unittest.main()
