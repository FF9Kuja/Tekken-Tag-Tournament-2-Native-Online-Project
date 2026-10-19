"""Tests for recomp/tools/audit_switches.py on synthetic images and hand-written generated code in
ReXGlue's format (one `// <mnemonic>` comment per instruction, `loc_` labels, the bctr switch)."""
import importlib.util
import json
import os
import tempfile
import unittest

import test_find_switch_tables as T

HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location(
    "audit_switches", os.path.join(os.path.dirname(os.path.dirname(HERE)), "recomp", "tools", "audit_switches.py"))
AUDIT = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(AUDIT)


def dispatch_image():
    """One function: cmplwi cr6,r3,3; bgt; lis/addi r11; slwi r10,r3,2; lwzx r11,r10,r11; mtctr; bctr; 4 cases."""
    img = T.Image()
    start = T.TEXT
    cases = [start + 4 * (8 + 2 * n) for n in range(4)]
    default = start + 4 * 16
    tbl = img.alloc(16)
    for n, lbl in enumerate(cases):
        img.put(tbl + 4 * n, lbl)
    words = [T.cmplwi(6, 3, 3), T.bc(12, 25, default - (start + 4)), T.lis(11, T.ha(tbl)), T.addi(11, 11, T.lo(tbl)),
             T.slwi(10, 3, 2), T.lwzx(11, 10, 11), T.mtctr(11), T.BCTR]
    for n in range(4):
        words += [T.li(3, n), T.BLR]
    words += [T.li(3, -1), T.BLR]
    T._function(img, words)
    return img, start, start + 28, cases, tbl


def generated(start, cases, switch_reg=None, labels=None, skip_comment=False):
    """The generated C++ for dispatch_image's function, as ReXGlue writes it."""
    name = f"sub_{start:08X}"
    mnems = ["cmplwi cr6,r3,3", "bgt cr6,0x...", "lis r11,...", "addi r11,r11,...", "rlwinm r10,r3,2,0,29",
             "lwzx r11,r10,r11", "mtctr r11", "bctr "]
    if skip_comment:
        del mnems[2]
    lines = [f"DEFINE_REX_FUNC({name}) {{", "\tREX_FUNC_PROLOGUE();"]
    for m in mnems:
        lines.append(f"\t// {m}")
        if m != "bctr ":
            lines.append("\tctx.r0.u64 = 0;")
    if switch_reg is None:
        lines += ["\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);", "\treturn;"]
    else:
        lines.append(f"\tswitch (ctx.r{switch_reg}.u32) {{")
        for n, lbl in enumerate(labels or cases):
            lines += [f"\tcase {n}:", f"\t\tgoto loc_{lbl:X};"]
        lines += ["\tdefault:", "\t\t__builtin_trap(); // Switch case out of range", "\t}"]
    for n, at in enumerate(cases):
        lines += [f"loc_{at:X}:", f"\t// li r3,{n}", "\tctx.r3.s64 = 0;", "\t// blr ", "\treturn;"]
    lines.append("}")
    return name, "\n".join(lines) + "\n"


def write_generated(d, start, text, name):
    with open(os.path.join(d, "ttt2_recomp.0.cpp"), "w") as f:
        f.write(text)
    with open(os.path.join(d, "ttt2_register.cpp"), "w") as f:
        f.write(f"void ttt2_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar) {{\n"
                f"  registrar->SetFunction(0x{start:08X}, {name});\n}}\n")


class AuditTests(unittest.TestCase):
    def run_audit(self, img, start, text, name, finder_json):
        with tempfile.TemporaryDirectory() as d:
            write_generated(d, start, text, name)
            return AUDIT.audit_image(img.finder(), d, finder_json)

    def finder_json(self, img):
        found, unresolved, other, unbounded = img.finder().run()
        hexed = lambda v: f"0x{v:08X}"
        return {"tables": [{"bctr": hexed(t["bctr"]), "register": t["register"],
                            "labels": [hexed(x) for x in t["labels"]]} for t in found],
                "unresolved_sites": [{"bctr": hexed(u["bctr"]), "reason": u["reason"], "sequence": []} for u in unresolved],
                "unbounded_sites": []}

    def test_finder_table_emitted_as_written(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=3)
        res = self.run_audit(img, start, text, name, self.finder_json(img))
        c = res["counts"]
        self.assertEqual((c["bctr_in_generated_code"], c["finder_tables_matching"], c["sdk_only_switches"],
                          c["mapping_problems"]), (1, 1, 0, 0))

    def test_generated_switch_differs_from_the_finder(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=10)
        res = self.run_audit(img, start, text, name, self.finder_json(img))
        self.assertEqual(len(res["mismatches"]), 1)
        self.assertIn("switch on r10", res["mismatches"][0]["problem"])

    def test_sdk_switch_on_the_loaded_target_is_a_miscompile(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=11)
        res = self.run_audit(img, start, text, name, {"tables": [], "unresolved_sites": [], "unbounded_sites": []})
        self.assertEqual(res["counts"]["sdk_only_miscompiles"], 1)
        self.assertTrue(res["sdk_only"][0]["verdict"].startswith("miscompile: the switch tests a value read from"))

    def test_sdk_switch_on_the_index_is_checked_case_by_case(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=3)
        res = self.run_audit(img, start, text, name, {"tables": [], "unresolved_sites": [], "unbounded_sites": []})
        self.assertTrue(res["sdk_only"][0]["verdict"].startswith("consistent"))
        name, text = generated(start, cases, switch_reg=3, labels=[cases[1], cases[0], cases[2], cases[3]])
        res = self.run_audit(img, start, text, name, {"tables": [], "unresolved_sites": [], "unbounded_sites": []})
        self.assertIn("miscompile: case 0 goes to", res["sdk_only"][0]["verdict"])

    def test_unread_dispatch_emitted_as_indirect_call(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=None)
        finder = {"tables": [], "unbounded_sites": [],
                  "unresolved_sites": [{"bctr": f"0x{bctr:08X}", "reason": "dispatch sequence not recognised: x",
                                        "sequence": ["0x82010000  cmplwi cr6,r3,3"]}]}
        res = self.run_audit(img, start, text, name, finder)
        self.assertEqual((res["counts"]["indirect"], res["counts"]["finder_unresolved_indirect"]), (1, 1))
        md = AUDIT.markdown(res)
        self.assertIn(f"### 0x{bctr:08X} in {name}: indirect", md)
        self.assertIn("traps if the game reaches it", md)
        self.assertIn("cmplwi cr6,r3,3", md)

    def test_lost_instruction_comment_is_a_mapping_problem(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=3, skip_comment=True)
        res = self.run_audit(img, start, text, name, self.finder_json(img))
        self.assertEqual(res["counts"]["mapping_problems"], 1)
        self.assertIn("does not hold a bctr", res["problems"][0])

    def test_json_addresses_are_hex(self):
        img, start, bctr, cases, _ = dispatch_image()
        name, text = generated(start, cases, switch_reg=11)
        res = self.run_audit(img, start, text, name, {"tables": [], "unresolved_sites": [], "unbounded_sites": []})
        out = json.loads(json.dumps(AUDIT._jsonable(res)))
        self.assertEqual(out["sdk_only"][0]["bctr"], f"0x{bctr:08X}")


if __name__ == "__main__":
    unittest.main()
