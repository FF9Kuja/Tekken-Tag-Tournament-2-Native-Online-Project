#!/usr/bin/env python3
"""Reports which PowerPC instructions of a list have a builder in the ReXGlue codegen.

ReXGlue translates each instruction through a builder registered in
src/codegen/instruction_dispatch.cpp; an instruction its disassembler decodes but no builder
handles becomes a REX_UNIMPLEMENTED trap in the generated code. The default list,
recomp/ttt2_instructions.txt, holds the instructions TTT2 is known to need.

    python3 -I scripts/check_instruction_coverage.py [--sdk DIR] [--list FILE] [--json OUT]

Exit 0 when the number of missing instructions equals --expect-missing (default 0), else 1;
2 on usage errors.
"""
import argparse
import json
import os
import re
import sys

RE_BUILDER = re.compile(r"\{\s*PPC_INST_(\w+)\s*,\s*build_\w+\s*\}")
RE_KNOWN = re.compile(r"\bPPC_INST_(\w+)\b")


def read_list(path):
    with open(path, encoding="utf-8") as f:
        return [ln.split("#", 1)[0].strip().lower() for ln in f if ln.split("#", 1)[0].strip()]


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", default=os.path.join(root, "third_party", "rexglue-sdk", "src"))
    ap.add_argument("--list", default=os.path.join(root, "recomp", "ttt2_instructions.txt"))
    ap.add_argument("--json")
    ap.add_argument("--expect-missing", type=int, default=0,
                    help="number of listed instructions known to lack a builder (exit 0 when it matches)")
    args = ap.parse_args(argv[1:])
    dispatch = os.path.join(args.sdk, "src", "codegen", "instruction_dispatch.cpp")
    if not os.path.isfile(dispatch):
        print(f"not found: {dispatch} (run scripts/fetch-rexglue-sdk.sh first)", file=sys.stderr)
        return 2
    with open(dispatch, encoding="utf-8") as f:
        builders = {m.lower() for m in RE_BUILDER.findall(f.read())}
    known = set()
    table = os.path.join(args.sdk, "thirdparty", "disasm", "ppc-inst.h")
    if os.path.isfile(table):
        with open(table, encoding="utf-8", errors="replace") as f:
            known = {m.lower() for m in RE_KNOWN.findall(f.read())}
    wanted = read_list(args.list)
    missing = [m for m in wanted if m not in builders]
    print(f"SDK builders: {len(builders)}; listed: {len(wanted)}; implemented: {len(wanted) - len(missing)}; missing: {len(missing)}")
    for m in missing:
        why = "decodable, no builder (REX_UNIMPLEMENTED at run time)" if m in known else "unknown to the disassembler"
        print(f"  missing {m}: {why}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"sdk_builders": len(builders), "listed": wanted,
                       "missing": [{"mnemonic": m, "decodable": m in known} for m in missing]}, f, indent=2)
    return 0 if len(missing) == args.expect_missing else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
