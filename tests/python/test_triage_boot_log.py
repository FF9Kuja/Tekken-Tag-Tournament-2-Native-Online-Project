"""Unit tests for recomp/tools/triage_boot_log.py on synthetic host logs shaped like the real ones
(line formats from recomp/src/ttt2_app.cpp and the ReXGlue runtime; no game data)."""
import importlib.util
import os
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("triage_boot_log", os.path.join(ROOT, "recomp", "tools", "triage_boot_log.py"))
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

P = "[2026-10-19 07:58:40.{ms:03d}] [{level}] [core] [t{tid}] "

COMMON_HEAD = [
    P.format(ms=836, level="info", tid=6976) + "ttt2: graphics backend {backend}",
    P.format(ms=838, level="info", tid=6976).replace("[core]", "[sys]")
    + "Guest memory arena mapped: virtual base 0x0000000100000000, physical base 0x0000000200000000",
    P.format(ms=949, level="warning", tid=6976).replace("[core]", "[sys]") + "Too few processor cores - scheduling will be wonky",
    P.format(ms=954, level="info", tid=6976) + "ttt2: executable title {title} version {version} (base 0.0.0.2), media 00000000",
    P.format(ms=954, level="info", tid=6976) + "ttt2: game data root   /home/me/game",
    P.format(ms=954, level="info", tid=6976) + "ttt2: imports 2 entries from xam",
    P.format(ms=954, level="info", tid=6976) + "ttt2: imports 1 entries from xboxkrnl",
    P.format(ms=955, level="info", tid=6976) + "ttt2: main guest thread created (id 4), starting",
]


def head(backend="none", title="54455354", version="0.0.0.0"):
    return [h.format(backend=backend, title=title, version=version) for h in COMMON_HEAD]


CLEAN = head() + [
    P.format(ms=955, level="info", tid=6982) + "ttt2: kernel override XamShowFriendsUI -> ERROR_SUCCESS, dismissed (first call)",
    P.format(ms=955, level="info", tid=6982) + "ttt2: kernel override XamUserCreateStatsEnumerator -> ERROR_NO_SUCH_USER (first call)",
    P.format(ms=955, level="info", tid=6984) + "ttt2: main guest thread exited (id 4)",
    P.format(ms=955, level="info", tid=6984) + "ttt2: watchdog: 4 guest threads (3 running); main thread not running: #1, #2, #3, #4; kernel stubs: 0 calls to 0 stubs",
    P.format(ms=955, level="info", tid=6984) + "ttt2: kernel stub report at main thread exit: 0 calls to 0 distinct stubs",
    P.format(ms=955, level="info", tid=6984) + "ttt2: guest program ended; exiting with code 0",
]

CRASH = head(backend="xenos", title="4E4D0859", version="0.0.3.2") + [
    P.format(ms=960, level="warning", tid=6625).replace("[core]", "[krnl]") + "__imp__XamShowKeyboardUI STUB",
    P.format(ms=961, level="warning", tid=6625).replace("[core]", "[krnl]") + "__imp__XamShowKeyboardUI STUB",
    P.format(ms=962, level="warning", tid=6625).replace("[core]", "[krnl]") + "__imp__XNetStartup STUB",
    P.format(ms=970, level="critical", tid=6625) + "ttt2: HOST CRASH: write fault at host address 6176112644 at host pc 0x55bc4719aa96",
    P.format(ms=970, level="critical", tid=6625) + "ttt2:   fault address 0x170110004",
    P.format(ms=970, level="critical", tid=6625) + "ttt2:   on guest thread ? (id 4)",
    P.format(ms=970, level="critical", tid=6625) + "ttt2:   kernel stubs so far: 3 calls to 2 stubs; top: XamShowKeyboardUI x2, XNetStartup x1",
    P.format(ms=970, level="critical", tid=6625) + "ttt2:   most recent stub calls: XamShowKeyboardUI, XamShowKeyboardUI, XNetStartup",
    P.format(ms=970, level="critical", tid=6625) + "ttt2: exiting with code 3 after the host crash",
]

STALL = head(backend="xenos", title="4E4D0859", version="0.0.3.2") + [
    P.format(ms=956, level="info", tid=7001) + "ttt2: watchdog: 20 guest threads (18 running); main thread running: nuscMain, #2; kernel stubs: 12 calls to 1 stubs; top: XamUserGetSigninState x12",
    P.format(ms=957, level="info", tid=7001) + "ttt2: watchdog: 20 guest threads (18 running); main thread running: nuscMain, #2; kernel stubs: 12 calls to 1 stubs; top: XamUserGetSigninState x12",
    P.format(ms=958, level="info", tid=7001) + "ttt2: watchdog: 20 guest threads (18 running); main thread running: nuscMain, #2; kernel stubs: 12 calls to 1 stubs; top: XamUserGetSigninState x12",
    P.format(ms=959, level="warning", tid=7001) + "ttt2: possible stall: thread census and stub calls unchanged for 15000 ms; per-thread state:",
    P.format(ms=959, level="warning", tid=7001) + "ttt2:   nuscMain                         id 4      running priority 0 suspend 0 wait_result 0x0",
    P.format(ms=959, level="warning", tid=7001) + "ttt2:   (unnamed)                        id 5      stopped priority 0 suspend 1 wait_result 0x102",
    P.format(ms=959, level="error", tid=7001).replace("[core]", "[sys]") + "Unhandled guest access violation: write of guest 0x70110004 (host 0x0000000170110004) on thread 0xF8000020",
    P.format(ms=959, level="error", tid=7001).replace("[core]", "[sys]") + "Unhandled guest access violation: write of guest 0x70110004 (host 0x0000000170110004) on thread 0xF8000020",
    P.format(ms=960, level="warning", tid=7001).replace("[core]", "[sys]") + "Variable import xboxkrnl:0xad (KeTimeStampBundle) not implemented",
]

STALL_THEN_CLOSED = STALL + [
    P.format(ms=970, level="info", tid=6976).replace("[core]", "[app]") + "Window closing, shutting down...",
    P.format(ms=970, level="info", tid=6976) + "ttt2: kernel stub report at window close: 12 calls to 1 distinct stubs",
    P.format(ms=970, level="info", tid=6976) + "ttt2:   XamUserGetSigninState 12 calls (first seen as stub call #1)",
]

TTT2WEB = {"XamShowKeyboardUI": ("runtime/kernel/xam_ui.cpp:200", "implemented")}


class Triage(unittest.TestCase):
    def test_clean_exit(self):
        t = MOD.triage(CLEAN, TTT2WEB)
        self.assertTrue(t["outcome"].startswith("clean exit"))
        self.assertEqual(t["facts"]["title_id"], "54455354")
        self.assertEqual(t["facts"]["graphics_backend"], "none")
        self.assertEqual(t["facts"]["imports"], {"xam": 2, "xboxkrnl": 1})
        self.assertEqual([o["name"] for o in t["overrides"]], ["XamShowFriendsUI", "XamUserCreateStatsEnumerator"])
        self.assertEqual(t["timeline"]["exit_code"], 0)
        self.assertEqual(t["stubs"]["list"], [])
        self.assertIsNone(t["crash"])
        recs = " ".join(t["recommendations"])
        self.assertIn("not Tekken Tag Tournament 2", recs)
        self.assertIn("GPU emulation was off", recs)
        md = MOD.render_markdown(t, "x.log")
        self.assertIn("clean exit", md)
        self.assertIn("XamShowFriendsUI -> ERROR_SUCCESS", md)

    def test_crash_with_stub_warnings(self):
        t = MOD.triage(CRASH, TTT2WEB)
        self.assertTrue(t["outcome"].startswith("crash: write fault"))
        self.assertEqual(t["crash"]["host_pc"], "0x55bc4719aa96")
        self.assertEqual(t["crash"]["fault_address"], "0x170110004")
        self.assertEqual(t["crash"]["exit_code"], 3)
        self.assertEqual(t["facts"]["title_id"], "4E4D0859")
        # No stub report in a crash log: the STUB warnings are the source.
        self.assertEqual(t["stubs"]["source"], "STUB warnings in the log")
        self.assertEqual([(s["name"], s["calls"]) for s in t["stubs"]["list"]], [("XamShowKeyboardUI", 2), ("XNetStartup", 1)])
        self.assertEqual(t["stubs"]["list"][0]["ttt2web"], "runtime/kernel/xam_ui.cpp:200")
        self.assertIsNone(t["stubs"]["list"][1]["ttt2web"])
        recs = t["recommendations"]
        self.assertTrue(any("guest virtual address 0x70110004" in r and "guest thread stacks" in r for r in recs))
        self.assertTrue(any("XamShowKeyboardUI reached 2x" in r and "xam_ui.cpp:200" in r for r in recs))
        self.assertTrue(any("XNetStartup reached 1x" in r and "does not define it" in r for r in recs))
        self.assertFalse(any("not Tekken Tag Tournament 2" in r for r in recs))

    def test_stall_access_violations_and_variables(self):
        t = MOD.triage(STALL, {})
        self.assertTrue(t["outcome"].startswith("stall"))
        self.assertEqual(len(t["stalls"]), 1)
        self.assertEqual(len(t["stalls"][0]["threads"]), 2)
        self.assertEqual(t["stalls"][0]["threads"][1]["wait_result"], "0x102")
        self.assertEqual(t["watchdog"]["ticks"], 3)
        self.assertEqual(t["access_violations"]["total"], 2)
        self.assertEqual(t["access_violations"]["first"]["guest"], "0x70110004")
        self.assertEqual(t["variable_imports_not_implemented"][0]["name"], "KeTimeStampBundle")
        recs = " ".join(t["recommendations"])
        self.assertIn("2 unhandled guest access violation", recs)
        self.assertIn("KeTimeStampBundle", recs)
        self.assertIn("Stall after 15000 ms", recs)
        self.assertEqual(t["errors"][0]["count"], 2)

    def test_stall_then_window_closed(self):
        t = MOD.triage(STALL_THEN_CLOSED, {})
        self.assertTrue(t["outcome"].startswith("stall"))
        self.assertIn("closed afterwards", t["outcome"])
        self.assertIsNotNone(t["timeline"]["closed"])
        self.assertEqual(len(t["stalls"]), 1)
        self.assertEqual(t["stubs"]["source"], "kernel stub report at window close")
        self.assertEqual([(s["name"], s["calls"]) for s in t["stubs"]["list"]], [("XamUserGetSigninState", 12)])
        md = MOD.render_markdown(t, "x.log")
        self.assertIn("closed afterwards", md)

    def test_call_to_unregistered_function(self):
        lines = [
            P.format(ms=100, level="info", tid=1) + "ttt2: executable title 4E4D0859 version 0.0.0.2 (base 0.0.0.2), media 5D762D06",
            P.format(ms=200, level="info", tid=1) + "ttt2: main guest thread created (id 7)",
            P.format(ms=300, level="critical", tid=2) + "[FATAL] Call to invalid or unregistered function at guest address 0x82b10a28",
            P.format(ms=301, level="critical", tid=2) + "ttt2: the runtime aborted (the [FATAL] line above says why); exiting with code 4",
        ]
        t = MOD.triage(lines)
        self.assertTrue(t["outcome"].startswith("fatal: the game called 0x82B10A28 through a pointer"))
        self.assertEqual([c["address"] for c in t["invalid_calls"]], ["0x82B10A28"])
        self.assertTrue(any("resolve_functions.py boot" in r for r in t["recommendations"]))
        md = MOD.render_markdown(t, "x.log")
        self.assertIn("## Calls to unregistered functions (runtime trap)", md)
        self.assertIn("indirect call to 0x82B10A28", md)

    def test_address_helpers(self):
        arena = (0x100000000, 0x200000000)
        self.assertEqual(MOD.guest_address(0x170110004, arena), ("virtual", 0x70110004))
        self.assertEqual(MOD.guest_address(0x2A0001000, arena), ("physical", 0xA0001000))
        self.assertIsNone(MOD.guest_address(0x55bc4719aa96, arena))
        self.assertIn("stacks", MOD.region_of(0x70110004))
        self.assertIn("executable image", MOD.region_of(0x82010000))
        self.assertIn("null page", MOD.region_of(0x10))

    def test_session_files_rotation_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            # an older session (old_000) and the newest one with two rotated parts; mtimes in that order
            names = ["old_000.log", "ttt2_001.2.log", "ttt2_001.1.log", "ttt2_001.log"]
            for i, n in enumerate(names):
                with open(os.path.join(tmp, n), "w") as f:
                    f.write("x\n")
                os.utime(os.path.join(tmp, n), (time.time() - 1000 + i * 10, time.time() - 1000 + i * 10))
            files = [os.path.basename(f) for f in MOD.session_files(tmp)]
            self.assertEqual(files, ["ttt2_001.2.log", "ttt2_001.1.log", "ttt2_001.log"])
            self.assertEqual(MOD.session_files(os.path.join(tmp, "old_000.log")), [os.path.join(tmp, "old_000.log")])


if __name__ == "__main__":
    unittest.main()
