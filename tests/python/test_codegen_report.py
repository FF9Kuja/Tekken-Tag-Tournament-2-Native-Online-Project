"""Unit tests for recomp/tools/codegen_report.py on synthetic codegen output (no game data)."""
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "recomp" / "tools" / "codegen_report.py"

spec = importlib.util.spec_from_file_location("codegen_report", TOOL)
codegen_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codegen_report)

FUNCS_H = "#pragma once\nDECLARE_REX_FUNC(xstart);\nDECLARE_REX_FUNC(sub_82010050);\nDECLARE_REX_FUNC(__imp__NtClose);\nDECLARE_REX_FUNC(rexcrt_memcpy);\n"
INIT_CPP = "PPCFuncMapping PPCFuncMappings[] = {\n\t{ 0x82010000, xstart },\n\t{ 0x82010050, sub_82010050 },\n\t{ 0, nullptr }\n};\n"
RECOMP_0 = """#include "t_funcs.0.h"

DEFINE_REX_FUNC(sub_82010050) {
	REX_FUNC_PROLOGUE();
	// cmplwi r3,2
	ctx.cr0.compare<uint32_t>(ctx.r3.u32, 2, ctx.xer);
	// lis r11,-32254
	ctx.r11.s64 = -2113798144;
	// lwzx r11,r10,r11
	ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r11.u32);
	// bctr 
	switch (ctx.r3.u32) {
	case 0:
		goto loc_82010070;
	case 1:
		goto loc_82010078;
	case 2:
		__builtin_trap(); // ERROR - detected jump to null value
	default:
		__builtin_trap(); // Switch case out of range
	}
	// UNIMPLEMENTED: macchwu
	REX_UNIMPLEMENTED(0x82010100, "macchwu");
	// UNIMPLEMENTED: maclhwu
	REX_UNIMPLEMENTED(0x82010104, "maclhwu");
	// ERROR: unresolved bl target 0x82FFFFF0
	REX_FATAL("Unresolved call from 0x82010108 to 0x82FFFFF0");
}
"""
LOG = """[2026-10-19 02:00:00.000] [codegen] [debug] Analyze: loaded 3 imports (2 resolved, 1 unresolved, 0 variables skipped)
[2026-10-19 02:00:00.001] [codegen] [error] Cannot resolve ordinal 4242 from xboxkrnl.exe
[2026-10-19 02:00:00.002] [codegen] [error] Jump table detection failed at bctr 0x82010200: mtctr=true, load=true, table_lis=true, table_addi=false, table_reg=r11, base_lis=false, base_addi=false
[2026-10-19 02:00:00.003] [codegen] [warning] Unimplemented: macchwu at 0x82010100
[2026-10-19 02:00:00.004] [codegen] [error] === ANALYSIS ERRORS ===
[2026-10-19 02:00:00.004] [codegen] [error] UnresolvedCall (2):
[2026-10-19 02:00:00.004] [codegen] [error]   0x82FFFFF0 from 0x82010108: bl 0x82FFFFF0 from 0x82010108 - target not in any function
[2026-10-19 02:00:00.004] [codegen] [error]   0x82FFFFF4 from 0x8201010C: b 0x82FFFFF4 from 0x8201010C - target not in any function
[2026-10-19 02:00:00.004] [codegen] [error] Total: 2 errors
[2026-10-19 02:00:00.005] [codegen] [error] Code generation blocked: 2 validation errors. Use --force to override.
[2026-10-19 02:00:00.006] [core] [info] Codegen summary: 7 written, 0 unchanged, 1 deleted, 0 module(s) up to date
"""


class CodegenReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.gen = pathlib.Path(self.tmp.name) / "generated"
        self.gen.mkdir()
        (self.gen / "t_funcs.h").write_text(FUNCS_H)
        (self.gen / "t_init.cpp").write_text(INIT_CPP)
        (self.gen / "t_recomp.0.cpp").write_text(RECOMP_0)
        self.log = pathlib.Path(self.tmp.name) / "codegen.log"
        self.log.write_text(LOG)

    def tearDown(self):
        self.tmp.cleanup()

    def test_generated_code_analysis(self):
        rep = codegen_report.analyse_generated(str(self.gen), "t")
        self.assertEqual(rep["functions"], 4)
        self.assertEqual(rep["imports"], 1)
        self.assertEqual(rep["rexcrt"], 1)
        self.assertEqual(rep["mappings"], 2)  # the { 0, nullptr } terminator is not counted
        self.assertEqual(len(rep["switches"]), 1)
        self.assertEqual(rep["switches"][0]["cases"], 3)
        self.assertEqual(rep["switches"][0]["register"], "r3")
        self.assertEqual([u["mnemonic"] for u in rep["unimplemented"]], ["macchwu", "maclhwu"])
        self.assertEqual(rep["unresolved_bl"], ["0x82FFFFF0"])
        self.assertEqual(rep["null_jumps"], 1)
        self.assertEqual(len(rep["fatals"]), 1)
        self.assertEqual(rep["definitions"], 1)
        self.assertEqual(rep["mnemonics"], {"cmplwi": 1, "lis": 1, "lwzx": 1, "bctr": 1})

    def test_log_analysis(self):
        log = codegen_report.analyse_log(str(self.log))
        self.assertEqual(log["imports"], {"total": 3, "resolved": 2, "unresolved": 1, "variables": 0})
        self.assertEqual(log["unresolved_ordinals"], [{"ordinal": 4242, "library": "xboxkrnl.exe"}])
        self.assertEqual(log["jump_table_failures"], ["0x82010200"])
        self.assertEqual(log["errors_by_category"], {"UnresolvedCall": 2})
        self.assertEqual(len(log["error_entries"]), 2)
        self.assertEqual(log["error_entries"][1]["from"], "0x8201010C")
        self.assertEqual(log["blocked_validation_errors"], 2)
        self.assertEqual(log["summary"], {"written": 7, "unchanged": 0, "deleted": 1})
        self.assertEqual(log["unimplemented"], [{"mnemonic": "macchwu", "address": "0x82010100"}])

    def test_cli_checks_and_outputs(self):
        md = pathlib.Path(self.tmp.name) / "REPORT.md"
        js = pathlib.Path(self.tmp.name) / "report.json"
        cmd = [sys.executable, "-I", str(TOOL), str(self.gen), "--log", str(self.log), "--project", "t",
               "--md", str(md), "--json", str(js), "--quiet", "--expect-functions", "4", "--expect-switches", "1",
               "--expect-unimplemented", "2"]
        self.assertEqual(subprocess.run(cmd).returncode, 0)
        data = json.loads(js.read_text())
        self.assertEqual(data["generated"]["functions"], 4)
        self.assertEqual(data["generated"]["unimplemented_histogram"][0]["count"], 1)
        text = md.read_text()
        self.assertIn("| Functions declared | 4 |", text)
        self.assertIn("`macchwu`", text)
        self.assertIn("UnresolvedCall", text)
        # --fail-on-errors must fail: unresolved call + analysis errors + a detection failure.
        self.assertEqual(subprocess.run(cmd + ["--fail-on-errors"]).returncode, 1)
        # A wrong expectation fails too, and the report says which.
        self.assertEqual(subprocess.run(cmd[:-6] + ["--expect-functions", "5"]).returncode, 1)
        self.assertIn("FAIL", md.read_text())

    def test_expected_mnemonics(self):
        lst = pathlib.Path(self.tmp.name) / "insns.txt"
        lst.write_text("# list\nlis\nlwzx\nmacchwu\n")
        base = [sys.executable, "-I", str(TOOL), str(self.gen), "--project", "t", "--quiet", "--expect-mnemonics", str(lst)]
        self.assertEqual(subprocess.run(base).returncode, 1)
        self.assertEqual(subprocess.run(base + ["--absent-ok", "macchwu"]).returncode, 0)

    def test_no_game_bytes_in_report(self):
        # The report must be built from names/addresses only: no instruction words appear.
        md = pathlib.Path(self.tmp.name) / "REPORT.md"
        subprocess.run([sys.executable, "-I", str(TOOL), str(self.gen), "--project", "t", "--md", str(md), "--quiet"])
        self.assertNotIn("goto loc_", md.read_text())

    def test_usage_errors(self):
        self.assertEqual(subprocess.run([sys.executable, "-I", str(TOOL), str(self.gen) + "/missing"]).returncode, 2)
        empty = pathlib.Path(self.tmp.name) / "empty"
        empty.mkdir()
        self.assertEqual(subprocess.run([sys.executable, "-I", str(TOOL), str(empty)]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
