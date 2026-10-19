"""Unit tests for the IMPORT_LIBRARIES header that scripts/ppc_elf_to_xex.py adds to test images."""
import importlib.util
import os
import struct
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location("ppc_elf_to_xex", os.path.join(ROOT, "scripts", "ppc_elf_to_xex.py"))
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class ImportLibrariesHeader(unittest.TestCase):
    def test_layout_matches_the_loader(self):
        libs = [("xam.xex", [0x82020010, 0x82010040]), ("xboxkrnl.exe", [0x82020018])]
        blob = MOD.build_import_libraries(libs)
        total, strings_size, count = struct.unpack_from(">III", blob, 0)
        self.assertEqual(total, len(blob))
        self.assertEqual(count, 2)
        # names padded to 4 bytes: "xam.xex\0" (8) + "xboxkrnl.exe\0" (13 -> 16)
        self.assertEqual(strings_size, 24)
        self.assertEqual(blob[12:12 + strings_size], b"xam.xex\0xboxkrnl.exe\0\0\0\0")
        off = 12 + strings_size  # the loader starts the library entries at string_table.size + 12
        size, = struct.unpack_from(">I", blob, off)
        self.assertEqual(size, 0x28 + 4 * 2)
        lib_id, version, version_min, name_index, records = struct.unpack_from(">IIIHH", blob, off + 0x18)
        self.assertEqual((lib_id, name_index, records), (0, 0, 2))
        self.assertEqual(version, version_min)
        self.assertEqual(version >> 28, 2)  # kernel 2.0.x
        self.assertEqual(struct.unpack_from(">II", blob, off + 0x28), (0x82020010, 0x82010040))
        off += size
        size, = struct.unpack_from(">I", blob, off)
        self.assertEqual(size, 0x28 + 4)
        self.assertEqual(struct.unpack_from(">HH", blob, off + 0x24), (1, 1))
        self.assertEqual(struct.unpack_from(">I", blob, off + 0x28), (0x82020018,))
        self.assertEqual(off + size, total)

    def test_imports_file_and_symbol_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "prog.imports")
            with open(path, "w", encoding="utf-8") as f:
                f.write("# comment\nxam.xex: A_slot, A_thunk\n\nxboxkrnl.exe: B_thunk # trailing comment\n")
            libs = MOD.read_imports_file(path)
        self.assertEqual(libs, [("xam.xex", ["A_slot", "A_thunk"]), ("xboxkrnl.exe", ["B_thunk"])])
        symbols = [(0x82020000, 0, "NOTYPE", "A_slot"), (0x82010100, 0, "NOTYPE", "A_thunk"),
                   (0x82010110, 0, "NOTYPE", "B_thunk"), (0x82010000, 20, "FUNC", "_start")]
        self.assertEqual(MOD.resolve_imports(libs, symbols),
                         [("xam.xex", [0x82020000, 0x82010100]), ("xboxkrnl.exe", [0x82010110])])
        with self.assertRaises(SystemExit):
            MOD.resolve_imports([("xam.xex", ["missing"])], symbols)

    def test_malformed_imports_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "bad.imports")
            with open(path, "w", encoding="utf-8") as f:
                f.write("no colon here\n")
            with self.assertRaises(SystemExit):
                MOD.read_imports_file(path)


if __name__ == "__main__":
    unittest.main()
