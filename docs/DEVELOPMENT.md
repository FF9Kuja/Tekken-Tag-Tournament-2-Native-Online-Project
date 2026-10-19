# Development guide

## Prerequisites

* **Linux (Ubuntu 24.04 tested):** `scripts/setup-ubuntu.sh` installs clang, lld, gcc, cmake, ninja, python3.
* **Windows:** Visual Studio 2022 with "Desktop development with C++" (MSVC x64, CMake, Ninja); for the clang-cl
  preset also the "C++ Clang tools for Windows" component or an LLVM 18+ install.
* **macOS:** not a target; the tooling should still build with Apple Clang 15+ (untested).

## Build and test

```sh
# Linux
scripts/build.sh linux-clang          # configure + build + ctest (out/build/linux-clang)
scripts/build.sh linux-gcc
scripts/build.sh linux-clang -DTTT2_WARNINGS_AS_ERRORS=ON

# Windows (PowerShell)
scripts\build.ps1 -Preset windows-msvc -Config Release
scripts\build.ps1 -Preset windows-clang-cl
```

Equivalent raw commands: `cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>`.
Binaries land in `out/build/<preset>/bin/` (MSVC: `bin/<Config>/`).

Options: `TTT2_BUILD_TESTS` (ON), `TTT2_BUILD_TOOLS` (ON), `TTT2_WARNINGS_AS_ERRORS` (OFF; CI turns it on for
the Linux jobs; the Windows jobs will get it once their first run shows a clean log).

`ctest` runs the doctest unit tests, the Python unit tests, the tools' version/rejection smoke tests and four
end-to-end runs of `ttt2-validate` on synthetic fixtures: `tests/tools/make_validate_fixtures.cpp` writes a disc
image, two title-update packages and a folder tree into the build directory, `tests/tools/run_expect.cmake` runs
the tool and checks exit code, printed reasons and the files written to `--work`. All inputs are synthetic.

## Tools

* `ttt2-xexinfo <file.xex|.xexp> [--json] [--headers] [--no-hash]` - identify an Xbox 360 executable or title
  update without decrypting it. It prints the title ID and version, hashes the file and tells you whether the
  hashes match a known copy. Example on the owner's disc copy (do not send the file, just the output):

  ```
  ttt2-xexinfo "D:\TTT2\Default.xex"
  ttt2-xexinfo "D:\TTT2\TU_174Q22P_0000008000000.00000000000O2" --headers   # (after STFS extraction: Milestone 3)
  ```

* `ttt2-iso info|list|identify|extract <image.iso> ...` - reads an Xbox 360 disc image in place (XGD3/XGD2/XGD1
  or an extracted partition); `identify` finds `default.xex`, parses and hashes it (`--image-hash` also hashes
  the decrypted image); `extract` copies one file out.
* `ttt2-stfs info|identify|extract <package> ...` - reads a title-update package (LIVE/PIRS/CON); `identify`
  reports the contained `.xexp`'s source -> target version.
* `ttt2-xexpatch <base.xex> <patch.xexp> <out.xex> [--force]` - applies a title update (Clang builds only; needs
  `scripts/fetch-xenonrecomp.sh` or `.ps1` first). The plaintext headers of both files are checked against each
  other first (title ID, versions, descriptor windows); a pair that cannot match is refused unless `--force`.
* `ttt2-validate <image.iso> <title-update-file-or-folder> [--json] [--work DIR] [--no-patch]` - everything
  above in one command: identifies the disc executable and the update (a folder is searched for the package whose
  `Default.xexp` targets 0.0.3.2), runs the pre-check, applies the update in memory when the patcher is compiled
  in, and reports hashes, import counts and the save/restore helper addresses of the image the recompiler sees.
  `--no-patch` stops after the pre-check (the end-to-end tests use it).
* `ttt2-xexinfo --imports` names every kernel/XAM import (ordinal, thunk and slot address) from the export
  tables in `src/xex/xbox_exports_table.inc` (regenerate with `scripts/gen_xbox_exports.py` after bumping the
  XenonRecomp pin) and marks what the pinned ReXGlue runtime provides for each one (implemented, stub, missing)
  from `src/xex/rexglue_runtime_exports.inc` (regenerate with `scripts/gen_runtime_exports.py` after bumping the
  SDK pin); `--helpers` locates the eight register save/restore helpers by byte pattern.
* The owner's end-to-end instructions are in `docs/OWNER_CHECKLIST.md`; `scripts/owner-pipeline.ps1` runs them.

## Recompiler tooling (`recomp/tools`, `scripts`)

* `recomp/tools/codegen_report.py <generated dir> [--log codegen.log]` - summarises a `rexglue codegen` run
  (functions, imports, jump tables, unimplemented sites, analysis errors, instruction mix) as REPORT.md / JSON
  with counts and addresses only; `--expect-*` options turn it into a check. Unit tests: `tests/python`
  (`python3 -I -m unittest discover -s tests/python`).
* `recomp/tools/find_switch_tables.py <image.xex> --toml tables.toml` - finds switch jump tables by symbolic
  execution and writes ReXGlue `[[switch_tables]]` entries; include the file from the codegen config. Needed
  because SDK 0.10.0's own detector misses offset tables and picks wrong registers (`docs/RESEARCH.md` 8a).
* `scripts/check_instruction_coverage.py [--expect-missing 2]` - compares `recomp/ttt2_instructions.txt` with
  the SDK's instruction builders.
* `scripts/make-ppc-test-xex.sh <source.c|.s> <out.xex>` - builds a license-free PowerPC test program and packs
  it as an XEX2 image (assembly through the SDK's bundled binutils); writes `<out.xex>.syms`.
* `recomp/tests/host` - headless host: the ReXGlue runtime in tool mode plus one program's generated code;
  runs the program natively, checks the values it stores and reports the kernel stubs it reached
  (`--max-stub-calls N` turns more than N stub calls into a failure; `scripts/codegen-smoke-test.sh --exec`).

## Repository hygiene

`python3 scripts/check_repo_hygiene.py` scans tracked files for game content, generated code and secrets; CI runs
it on every push. Install it as a pre-commit hook with `scripts/install-git-hooks.sh`.

## Coding standards

* C++20, warnings clean on Clang, GCC and MSVC with the flags in `cmake/ttt2_warnings.cmake`.
* Format with `clang-format` (`.clang-format`, Google-based, 110 columns).
* Every parser reads through `ttt2::ByteReader`; no raw pointer arithmetic on untrusted input.
* Every new module comes with doctest tests under `tests/`; synthetic inputs only (never game bytes).
* Stubs, prototypes and unverified behaviour are labelled in code comments and in `docs/STATUS.md`.

## Continuous integration

`.github/workflows/ci.yml`: hygiene check and Python unit tests; Linux clang and gcc (`-Werror`); Windows MSVC (VS
2022 generator) and clang-cl (Ninja). All jobs build the tools and run the tests; Linux jobs also smoke-test the
tools. `sdk-validation.yml` (weekly/manual) builds the SDK, runs the codegen checks and the native run of the
switch-table program through the headless host (`--exec`); `rexglue-windows.yml` (weekly/manual) builds
`rexglue.exe` on Windows and, with the `native_run` input, also builds the headless host on Windows and runs the
program there (the first native-execution evidence on the target platform; slow).

## Working with the ReXGlue SDK on Linux (Milestone 3 preview)

Measured in the first session on Ubuntu 24.04 (the SDK wiki is right: **Clang 20** is needed on Linux - Clang 18
reports `__cpp_concepts 201907L`, so libstdc++ 13 hides `std::expected`, which `rex/result.h` uses). The SDK's
core also needs SSSE3 enabled (`-mssse3`; the tekken6-recomp README passes the same flag). Where apt.llvm.org is
unreachable, the LLVM GitHub release tarball works (`LLVM-20.1.8-Linux-X64.tar.xz`, 2 GB; symlink
`clang-20`/`clang++-20`/`ld.lld-20` into the PATH), which is how session 3 built the SDK.

```sh
sudo apt-get install -y clang-20 lld-20 libclang-rt-20-dev cmake ninja-build nasm \
    libgtk-3-dev libxss-dev libx11-xcb-dev libxcb1-dev libxcb-randr0-dev libxcb-xfixes0-dev \
    libxcb-shape0-dev libxcb-xkb-dev libxcb-icccm4-dev libxcb-keysyms1-dev libxcb-cursor-dev \
    libxkbcommon-x11-dev libxcb-util-dev libxcb-image0-dev libxcb-render-util0-dev libxcb-xinerama0-dev \
    libxcb-present-dev libxcb-dri3-dev libxcb-sync-dev libxcb-glx0-dev libxcb-shm0-dev libxcb-xinput-dev \
    libxinerama-dev libxcursor-dev libxi-dev libxrandr-dev libasound2-dev libpulse-dev libwayland-dev \
    libdecor-0-dev libxkbcommon-dev libdrm-dev libgbm-dev libibus-1.0-dev libudev-dev libegl1-mesa-dev \
    libgl1-mesa-dev libdbus-1-dev
git clone --recursive https://github.com/rexglue/rexglue-sdk   # pin the commit from docs/DEPENDENCIES.md
cmake -S rexglue-sdk -B build/sdk -G Ninja -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-mssse3 -DCMAKE_CXX_FLAGS=-mssse3 \
      -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF -DSDL_X11_XSCRNSAVER=OFF
cmake --build build/sdk --target rexglue --parallel        # the codegen tool only
```

Verified in the first session: the tool builds (809 steps) and `rexglue init`, `rexglue codegen` (loads ELF/XEX;
analysis needs a PE `.pdata` directory) and `rexglue recompile-tests` run. To regenerate the SDK's PowerPC
instruction tests as Catch2 sources without any game file:

```sh
scripts/sdk-recompile-tests.sh /path/to/rexglue-sdk /path/to/rexglue-sdk/out/linux-amd64/rexglue out/ppc-tests
```

End-to-end recompilation of license-free programs (`recomp/tests/ppc/*.c` -> XEX2 image -> `rexglue codegen`
-> generated C++ syntax-checked against the SDK):

```sh
scripts/codegen-smoke-test.sh /path/to/rexglue-sdk/out/linux-amd64/rexglue /path/to/rexglue-sdk out/codegen-smoke
```

`scripts/ppc_elf_to_xex.py` is the packer behind it (PE32 for PowerPC-BE with a `.pdata` exception directory
synthesised from the ELF's function symbols, wrapped in unencrypted, uncompressed XEX2 headers). Add `--exec` to
also build `recomp/tests/host` and run the switch-table program natively (slow: it builds the runtime). To
compile-check the real host (`recomp/`, `Ttt2App`) on Linux, put any test image at `recomp/game/default.xex` and
configure `recomp/` with `-DREXSDK_DIR=third_party/rexglue-sdk/src` (Clang 20, `-mssse3`).

## Windows build of the codegen tool

`scripts\build-rexglue-windows.ps1` follows the SDK's own CI (LLVM clang + Ninja, `win-amd64` preset; needs
Visual Studio 2022's C++ workload for the Windows SDK headers). `.github/workflows/rexglue-windows.yml` runs the
same steps on a Windows runner on demand and uploads `rexglue.exe` as an artifact, so the owner can also download
the tool instead of building it.

Without a Windows machine the PowerShell scripts are checked with the PowerShell 7 parser
(`pwsh -Command '[System.Management.Automation.Language.Parser]::ParseFile(...)'`) and the workflow files with
`actionlint`; both are cheap and worth running before a push, actionlint caught an invalid YAML step name once.
