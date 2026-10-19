#!/usr/bin/env python3
"""Repository hygiene check: refuses game content and secrets.

Scans the files git tracks (or the paths given on the command line) and fails
when any of them looks like Xbox 360 game content, generated game code, a
secret, or a large binary. Run by CI and by the optional pre-commit hook.

    python3 scripts/check_repo_hygiene.py            # all tracked files
    python3 scripts/check_repo_hygiene.py --staged   # files staged for commit
    python3 scripts/check_repo_hygiene.py path ...   # specific files

Exit code 0 = clean, 1 = findings, 2 = usage error.
"""
import os
import re
import subprocess
import sys

MAX_FILE_BYTES = 5 * 1024 * 1024  # anything bigger is almost certainly not source

FORBIDDEN_SUFFIXES = (
    ".iso", ".xex", ".xexp", ".xzp", ".xpr", ".xma", ".xwb", ".xsb", ".img", ".xdbf",
    ".dmp", ".pdb", ".exe", ".dll", ".lib", ".obj", ".pch", ".sav", ".pem", ".key", ".pfx", ".p12",
)
FORBIDDEN_NAME_PATTERNS = (
    re.compile(r"^TU_[0-9A-Z]+_", re.IGNORECASE),        # title update packages
    re.compile(r"^default\.xex", re.IGNORECASE),
    re.compile(r"^ppc_recomp", re.IGNORECASE),             # XenonRecomp output
    re.compile(r"^ppc_func_mapping", re.IGNORECASE),
    re.compile(r"^recomp\.\d+\.cpp$", re.IGNORECASE),      # ReXGlue output
    re.compile(r"^codegen\.(d|build\.stamp)$", re.IGNORECASE),
    re.compile(r"_(init|register|funcs|pch)\.(h|cpp)$", re.IGNORECASE),
    re.compile(r"^(dll_targets|sources)\.cmake$", re.IGNORECASE),
    re.compile(r"^(data|nrfo)\d*\.(bin|ofs)$", re.IGNORECASE),  # TTT2 disc data containers
)
# Magic bytes of the formats we must never ship.
MAGICS = (
    (b"XEX2", "Xbox 360 executable (XEX2)"),
    (b"XEX1", "Xbox 360 executable (XEX1)"),
    (b"LIVE", "STFS package (LIVE)"),
    (b"PIRS", "STFS package (PIRS)"),
    (b"CON ", "STFS package (CON)"),
    (b"MICROSOFT*XBOX*MEDIA", "XDVDFS disc image"),
)
SECRET_PATTERNS = (
    re.compile(rb"-----BEGIN (RSA |EC |OPENSSH |)PRIVATE KEY-----"),
    re.compile(rb"ghp_[A-Za-z0-9]{30,}"),
    re.compile(rb"github_pat_[A-Za-z0-9_]{30,}"),
    re.compile(rb"AKIA[0-9A-Z]{16}"),
    re.compile(rb"xox[baprs]-[A-Za-z0-9-]{10,}"),
)
# Recompiled Xbox 360 code is a wall of sub_82xxxxxx symbols. A handful in a
# document or a config file is fine; hundreds in one file is generated code.
GENERATED_CODE_SYMBOL = re.compile(rb"sub_8[0-9A-Fa-f]{7}")
GENERATED_CODE_THRESHOLD = 200

ALLOWLIST = {
    "third_party/doctest/doctest.h",  # large vendored header
}


def git_files(staged):
    args = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"] if staged else ["git", "ls-files"]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line]


def read_content(path, staged):
    """Bytes to scan: the index copy when checking a staged file, else the working tree."""
    if staged:
        result = subprocess.run(["git", "show", f":{path}"], capture_output=True)
        return result.stdout if result.returncode == 0 else None
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        return f.read(MAX_FILE_BYTES + 1)


def check_file(path, staged=False):
    findings = []
    rel = path.replace(os.sep, "/")
    if rel in ALLOWLIST:
        return findings
    name = os.path.basename(path)
    lower = name.lower()
    if lower.endswith(FORBIDDEN_SUFFIXES):
        findings.append("forbidden file type")
    for pattern in FORBIDDEN_NAME_PATTERNS:
        if pattern.search(name):
            findings.append("forbidden file name (game content or generated code)")
            break
    data = read_content(path, staged)
    if data is None:
        return findings
    size = len(data)
    if size > MAX_FILE_BYTES:
        findings.append(f"file is {size} bytes (limit {MAX_FILE_BYTES})")
    head = data[:64]
    for magic, what in MAGICS:
        if head.startswith(magic) or (magic in head and len(magic) > 8):
            findings.append(f"starts with the magic of a(n) {what}")
    for pattern in SECRET_PATTERNS:
        if pattern.search(data):
            findings.append("looks like it contains a credential or private key")
            break
    hits = len(GENERATED_CODE_SYMBOL.findall(data))
    if hits >= GENERATED_CODE_THRESHOLD:
        findings.append(f"contains {hits} recompiled-code symbols (generated game code?)")
    return findings


def main(argv):
    staged = "--staged" in argv
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if "--help" in argv or "-h" in argv:
        print(__doc__)
        return 2
    try:
        files = paths if paths else git_files(staged)
    except subprocess.CalledProcessError as e:
        print(f"git failed: {e}", file=sys.stderr)
        return 2
    problems = 0
    for path in files:
        for finding in check_file(path, staged=staged and not paths):
            problems += 1
            print(f"HYGIENE: {path}: {finding}")
    if problems:
        print(f"\n{problems} problem(s). Game content, generated game code and secrets must never be committed "
              "(docs/LEGAL.md).")
        return 1
    print(f"hygiene check: {len(files)} file(s) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
