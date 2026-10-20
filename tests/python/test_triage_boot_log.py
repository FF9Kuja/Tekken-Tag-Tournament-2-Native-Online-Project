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


def W(ms, text, level="info", tid=7001, category="core"):
    return P.format(ms=ms, level=level, tid=tid).replace("[core]", f"[{category}]") + text


# A boot the way the boot monitor (recomp/src/boot_monitor.cpp and the export wrappers) logs it: the disc sector
# query answered as unsupported, the bare disc device opened anyway, the GPU set up but no frame presented, then
# a deadlock of two threads over critical sections and a third thread on an event nobody sets.
ACTIVITY = ("ttt2: activity: files 14 opened, 3 failed (1 device paths); read 4.1 MB in 96 reads (0 failed); sector "
            "queries 5; threads created 27; gpu: engines initialized, ring buffer 0x1FF00000, interrupt callback "
            "0x82A3B4C0, vblanks 600, ring write pointer 0x40; frames presented 0; audio clients 1, audio frames 0; "
            "input polls 0; traced kernel calls 104532")
DUMP_THREADS = [
    "ttt2:   nuscMain                         id 4      running priority 0 suspend 0 wait_result 0x0",
    "ttt2:     wait: RtlEnterCriticalSection on critical section 0x40001000 (owned by nuscLoader (id 9), recursion 1), "
    "no timeout, for at least 10.0 s, called from 0x8234ABCC (sub_8234AB00+0xCC)",
    "ttt2:     kernel calls: 0 in the last tick, none for at least 10.0 s, 1534 in total; last RtlEnterCriticalSection "
    "from 0x8234ABCC (sub_8234AB00+0xCC)",
    "ttt2:     guest stack: 0x8234ABCC (sub_8234AB00+0xCC) <- 0x82301230 (sub_82301000+0x230)",
    "ttt2:   nuscLoader                       id 9      running priority 0 suspend 0 wait_result 0x0",
    "ttt2:     wait: RtlEnterCriticalSection on critical section 0x40002000 (owned by nuscMain (id 4), recursion 2), "
    "no timeout, for at least 10.0 s, called from 0x82400010 (sub_82400000+0x10)",
    "ttt2:     guest stack: 0x82400010 (sub_82400000+0x10)",
    "ttt2:   nuscAudio                        id 11     running priority 0 suspend 0 wait_result 0x0",
    "ttt2:     wait: KeWaitForSingleObject on 0x40003000 (synchronization event, not signaled), no timeout, for at "
    "least 10.0 s, called from 0x82500020 (sub_82500000+0x20)",
    "ttt2:   (unnamed)                        id 1      running priority 0 suspend 0 wait_result 0x0",
    "ttt2:     wait: not traced (this thread has made no traced kernel call)",
    "ttt2:   traced kernel calls in the last tick: KeWaitForSingleObject 3",
    "ttt2:   recent file activity (last 1, newest last):",
    "ttt2:     t+2.1 s NtCreateFile '\\Device\\Cdrom0' -> 0xC000000F (STATUS_NO_SUCH_FILE), nuscLoader (id 9), from 0x82400100 (sub_82400000+0x100)",
]
MONITOR = head(backend="xenos", title="4E4D0859", version="0.0.0.2") + [
    W(955, "ttt2: 47 kernel exports traced by the host (file access, waits, GPU, audio, input, dialogs, threads)"),
    W(956, "ttt2: thread created: id 9, start 0x82400000 (sub_82400000), context 0x40000000 (nuscMain (id 4), from 0x82300100 (sub_82300000+0x100))"),
    W(957, "ttt2: gpu: VdInitializeEngines -> 1 (nuscMain (id 4), from 0x82300200 (sub_82300000+0x200))"),
    W(957, "ttt2: gpu: ring buffer at 0x1FF00000, size 2^12 (nuscMain (id 4), from 0x82300210 (sub_82300000+0x210))"),
    W(958, "ttt2: file opened: 'game:\\data\\boot.bin' (NtCreateFile, nuscLoader (id 9))"),
    W(958, "ttt2: sector query: FileSectorInformation for '\\Device\\Harddisk0\\Partition1\\data\\boot.bin' answered as unsupported (0xC0000003 (STATUS_INVALID_INFO_CLASS)), as TTT2 Web answers it"),
    W(959, "ResolvePath(\\Device) failed - device not found", level="error", category="fs"),
    W(959, "ttt2: the game opened '\\Device\\Cdrom0', a device itself rather than a file on it; ReXGlue mounts the game folder", level="warning"),
    W(959, "ttt2: file open failed: '\\Device\\Cdrom0' -> 0xC000000F (STATUS_NO_SUCH_FILE) (NtCreateFile, nuscLoader (id 9), from 0x82400100 (sub_82400000+0x100))"),
    W(959, "ttt2: file open failed: 'cache:\\shader.bin' -> 0xC000000F (STATUS_NO_SUCH_FILE) (NtCreateFile, nuscMain (id 4), from 0x82300300 (sub_82300000+0x300))"),
    W(960, "ttt2: watchdog: 28 guest threads (28 running); main thread running: nuscMain, nuscLoader; kernel stubs: 0 calls to 0 stubs"),
    W(960, ACTIVITY),
    W(961, "ttt2: possible stall: thread census, stub calls and boot progress (files, reads, frames) unchanged for 15000 ms; per-thread state:", level="warning"),
] + [W(961, line, level="warning") for line in DUMP_THREADS] + [
    W(962, "ttt2: watchdog: 28 guest threads (28 running); main thread running: nuscMain, nuscLoader; kernel stubs: 0 calls to 0 stubs"),
    W(962, ACTIVITY),
    W(963, "ttt2: still stalled after 60000 ms; per-thread state:", level="warning"),
] + [W(963, line, level="warning") for line in DUMP_THREADS] + [
    W(970, "Window closing, shutting down...", category="app"),
    W(970, ACTIVITY),
    W(970, "ttt2: thread state at window close:"),
] + [W(970, line) for line in DUMP_THREADS] + [
    W(971, "ttt2: kernel stub report at window close: 0 calls to 0 distinct stubs"),
]


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

    def test_boot_monitor_stall(self):
        t = MOD.triage(MONITOR, {})
        self.assertTrue(t["outcome"].startswith("stall"))
        self.assertIn("closed afterwards", t["outcome"])
        self.assertEqual(t["traced_exports"], 47)
        # Two stall dumps (the second a "still stalled" one) and the dump written on window close.
        self.assertEqual([st["still"] for st in t["stalls"]], [False, True])
        self.assertEqual(t["close_dump"]["when"], "window close")
        main = t["close_dump"]["threads"][0]
        self.assertEqual((main["name"], main["id"]), ("nuscMain", 4))
        self.assertIn("owned by nuscLoader (id 9)", main["wait"])
        self.assertIn("1534 in total", main["calls"])
        self.assertIn("sub_82301000", main["stack"])
        self.assertEqual(t["close_dump"]["busiest"], "KeWaitForSingleObject 3")
        self.assertEqual(len(t["close_dump"]["files"]), 1)
        # The activity line, field by field.
        a = t["activity"]
        self.assertEqual((a["files_opened"], a["open_failures"], a["device_failures"]), (14, 3, 1))
        self.assertEqual((a["mb_read"], a["reads"], a["sector_queries"]), (4.1, 96, 5))
        self.assertEqual((a["ring_buffer"], a["interrupt_callback"], a["vblanks"]), ("0x1FF00000", "0x82A3B4C0", 600))
        self.assertEqual((a["frames_presented"], a["audio_clients"], a["traced_calls"]), (0, 1, 104532))
        self.assertTrue(a["gpu_emulation"])
        # File access.
        self.assertEqual([d["path"] for d in t["files"]["device_opens"]], ["\\Device\\Cdrom0"])
        self.assertEqual(t["files"]["failed"][0]["status"], "0xC000000F")
        self.assertEqual(t["files"]["failed"][0]["status_name"], "STATUS_NO_SUCH_FILE")
        self.assertIn("boot.bin", t["files"]["sector_first"]["path"])
        self.assertEqual(t["threads_created"][0]["function"], "sub_82400000")
        self.assertEqual([m["area"] for m in t["milestones"]], ["gpu", "gpu"])
        recs = t["recommendations"]
        self.assertTrue(any(r.startswith("Deadlock:") and "nuscMain (id 4)" in r and "nuscLoader (id 9)" in r for r in recs))
        self.assertTrue(any("although the host answered its disc sector query as unsupported" in r for r in recs))
        self.assertTrue(any("cache:\\shader.bin" in r for r in recs))
        self.assertTrue(any(r.startswith("No frame was presented") and "after the GPU set-up" in r for r in recs))
        self.assertTrue(any("wait without a timeout" in r and "nuscAudio" in r for r in recs))
        # The deadlocked pair is a cycle, not a chain.
        self.assertFalse(any(r.startswith("Wait chain") for r in recs))
        md = MOD.render_markdown(t, "x.log")
        for heading in ("## Boot progress", "## File access", "## Milestones", "## Threads created",
                        "## Possible stall", "## Still stalled", "## Thread state at window close"):
            self.assertIn(heading, md)
        self.assertIn("**Bare device opened**", md)
        self.assertIn("guest stack: 0x8234ABCC", md)

    def test_wait_chain_and_frames(self):
        lines = head(backend="xenos", title="4E4D0859", version="0.0.0.2") + [
            W(960, ACTIVITY.replace("frames presented 0", "frames presented 120")),
            W(970, "ttt2: thread state at window close:"),
            W(970, "ttt2:   nuscMain                         id 4      running priority 0 suspend 0 wait_result 0x0"),
            W(970, "ttt2:     wait: RtlEnterCriticalSection on critical section 0x40001000 (owned by thread id 9, recursion 1), no timeout"),
            W(970, "ttt2:   (unnamed)                        id 9      running priority 0 suspend 0 wait_result 0x0"),
            W(970, "ttt2:     wait: KeWaitForSingleObject on 0x40003000 (synchronization event, not signaled), no timeout"),
        ]
        t = MOD.triage(lines, {})
        recs = t["recommendations"]
        self.assertTrue(any(r.startswith("120 frame(s) were presented") for r in recs))
        chain = [r for r in recs if r.startswith("Wait chain")]
        self.assertEqual(len(chain), 1)
        self.assertIn("nuscMain (id 4) waits for a lock held by (unnamed) (id 9), which waits: KeWaitForSingleObject", chain[0])
        self.assertFalse(any(r.startswith("Deadlock") for r in recs))

    def test_device_open_without_sector_query(self):
        lines = head(backend="xenos", title="4E4D0859", version="0.0.0.2") + [
            W(959, "ttt2: the game opened '\\Device\\Cdrom0', a device itself rather than a file on it", level="warning"),
            W(959, "ttt2: file open failed: '\\Device\\Cdrom0' -> 0xC000000F (STATUS_NO_SUCH_FILE) (NtOpenFile, nuscMain (id 4), from 0x82400100 (sub_82400000+0x100))"),
        ]
        t = MOD.triage(lines, {})
        recs = t["recommendations"]
        self.assertTrue(any("without asking for a disc sector first" in r for r in recs))
        # The device's own failure is not repeated among the other failed paths.
        self.assertFalse(any("other path(s) failed to open" in r for r in recs))

    def test_parse_activity_without_gpu(self):
        a = MOD.parse_activity(
            "files 1 opened, 1 failed (1 device paths); read 0.0 MB in 1 reads (0 failed); sector queries 1; threads "
            "created 0; gpu: engines not initialized, ring buffer not set up, interrupt callback none, no GPU emulation; "
            "frames presented 0; audio clients 0, audio frames 0; input polls 0; traced kernel calls 6")
        self.assertFalse(a["gpu_emulation"])
        self.assertNotIn("vblanks", a)
        self.assertEqual((a["engines"], a["ring_buffer"], a["interrupt_callback"]), ("not initialized", "not set up", "none"))
        self.assertEqual(a["traced_calls"], 6)

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
