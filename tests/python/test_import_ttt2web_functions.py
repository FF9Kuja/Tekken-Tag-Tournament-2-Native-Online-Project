"""Unit tests for recomp/tools/import_ttt2web_functions.py on a synthetic TTT2 Web-style config."""
import importlib.util
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "import_ttt2web_functions", os.path.join(ROOT, "recomp", "tools", "import_ttt2web_functions.py"))
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

SAMPLE = """# XenonRecomp configuration (synthetic sample)
[main]
file_path = "../../game/files/default.xex"
restgprlr_14_address = 0x82BAF6A0
savegprlr_14_address = 0x82BAF650
restfpr_14_address = 0x82BB033C
savefpr_14_address = 0x82BB02F0
restvmx_14_address = 0x82BB1558
savevmx_14_address = 0x82BB12C0
restvmx_64_address = 0x82BB15EC
savevmx_64_address = 0x82BB1354

# functions = [
#     { address = 0x00000000, size = 0x0 },
# ]
invalid_instructions = [
    { data = 0x00000000, size = 4 },
]
# BEGIN GENERATED FUNCTIONS (tools/find_switch_tables.py)
functions = [
    { address = 0x82240088, size = 0x8 },
    { address = 0x822400D8, size = 0x14 },
    { address = 0x82240088, size = 0x10 },
    { address = 0x82240100, size = 0 },
]
# END GENERATED FUNCTIONS
"""


class ParseTests(unittest.TestCase):
    def test_entries_and_helpers(self):
        entries, helpers = MOD.parse(SAMPLE)
        # duplicates keep the larger size, zero-sized entries are dropped, the commented template is ignored
        self.assertEqual(entries, {0x82240088: 0x10, 0x822400D8: 0x14})
        self.assertEqual(len(helpers), 8)
        self.assertEqual(helpers["restgprlr_14"], 0x82BAF6A0)
        self.assertEqual(helpers["savevmx_64"], 0x82BB1354)

    def test_main_writes_rexglue_functions_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "TTT2.toml")
            out = os.path.join(tmp, "functions.toml")
            with open(src, "w", encoding="utf-8") as f:
                f.write(SAMPLE)
            self.assertEqual(MOD.main(["prog", src, "--out", out]), 0)
            with open(out, encoding="utf-8") as f:
                text = f.read()
        self.assertIn("[functions]", text)
        self.assertIn('"0x82240088" = { size = 0x10 }', text)
        self.assertIn('"0x822400D8" = { size = 0x14 }', text)
        self.assertNotIn("0x82240100", text)
        self.assertIn("restgprlr_14: 0x82BAF6A0", text)
        # the file must be valid TOML for ReXGlue's parser
        import tomllib
        data = tomllib.loads(text)
        self.assertEqual(data["functions"]["0x82240088"]["size"], 0x10)

    def test_no_entries_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "empty.toml")
            with open(src, "w", encoding="utf-8") as f:
                f.write("[main]\n")
            self.assertEqual(MOD.main(["prog", src, "--out", os.path.join(tmp, "o.toml")]), 2)


if __name__ == "__main__":
    unittest.main()
