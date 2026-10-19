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

`ctest` runs the doctest unit tests, the Python unit tests, the tools' version/rejection smoke tests and the
end-to-end runs on synthetic fixtures: `tests/tools/make_validate_fixtures.cpp` writes a disc image whose
`default.xex` is in the retail form (AES-encrypted, basic-compressed, like the owner's), that executable and the
TU3 delta patch as files, two title-update packages and a folder tree into the build directory;
`tests/tools/run_expect.cmake` runs a tool and checks exit code, printed reasons, the files written and the files
that must not exist. Covered: four `ttt2-validate` runs (matching pair, folder search, mismatch, no update), the
preparation step `ttt2-xexinfo --write-plain` (verified output with the known image SHA-1, plain output re-read
with the same imports and helpers, same-path refusal, delta-patch refusal), the switch-table finder refusing the
retail form and accepting the plain output, and `tests/tools/make_retail_xex.cpp` (the wrapper the SDK workflows
use to put a license-free program into the retail form) round-tripped. All inputs are synthetic.

## Tools

* `ttt2-xexinfo <file.xex|.xexp> [--json] [--headers] [--no-hash]` - identify an Xbox 360 executable or title
  update without decrypting it. It prints the title ID and version, hashes the file and tells you whether the
  hashes match a known copy. Example on the owner's disc copy (do not send the file, just the output):

  ```
  ttt2-xexinfo "D:\TTT2\Default.xex"
  ttt2-xexinfo "D:\TTT2\TU_174Q22P_0000008000000.00000000000O2" --headers   # (after STFS extraction: Milestone 3)
  ```

* `ttt2-iso info|list|identify|extract|extract-all <image.iso> ...` - reads an Xbox 360 disc image in place
  (XGD3/XGD2/XGD1 or an extracted partition); `identify` finds `default.xex`, parses and hashes it
  (`--image-hash` also hashes the decrypted image); `extract` copies one file out; `extract-all` writes the whole
  tree into a directory (the game data root the native host runs from; entry names are sanitised).
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
* `ttt2-xexinfo <file.xex> --write-plain <out.xex>` writes the plain form of a retail executable: the input the
  switch-table finder and `rexglue codegen` read (`src/xex/basefile.hpp`: `make_plain_xex`, `verify_plain_xex`).
  The headers are copied byte for byte except that FILE_FORMAT_INFO says encryption none / compression none, and
  the decrypted, basic-decompressed PE image follows them (the layout `ttt2-xexpatch` and XenonUtils write). The
  written file is read back and verified against the input (headers, image, sizes); a file that fails is deleted,
  the input is never written, and the tool refuses to name the input as output. LZX-compressed images and delta
  patches are refused with the reason. The owner pipeline runs this as step 5c and stops on failure.
* `ttt2-xexinfo --imports` names every kernel/XAM import (ordinal, thunk and slot address) from the export
  tables in `src/xex/xbox_exports_table.inc` (regenerate with `scripts/gen_xbox_exports.py` after bumping the
  XenonRecomp pin) and marks what the pinned ReXGlue runtime provides for each one (implemented, stub, missing)
  from `src/xex/rexglue_runtime_exports.inc` (regenerate with `scripts/gen_runtime_exports.py` after bumping the
  SDK pin); `--helpers` locates the eight register save/restore helpers by byte pattern.
* The owner's end-to-end instructions are in `docs/OWNER_CHECKLIST.md`; `scripts/owner-pipeline.ps1` runs them.
  The pipeline stops on a critical failure (`Fail-Pipeline`: a `CRITICAL:` line, the report written, a throw)
  wherever a later stage would otherwise run on missing files: disc/update not readable or not extractable, the
  plain image not prepared, the finder, codegen, the host build or the extraction failing. CI exercises the dry
  runs on the synthetic fixtures under Windows PowerShell 5.1 (TU mode, base mode with step 5c, the finder on
  both files of the work folder, and a stopped run on an input that is not a disc image).
* `ttt2-stfs extract-all <package> <dir>` extracts every file of an STFS package with its folders (a title
  update's data files, the host's `update:` root); ctest `stfs_extract_all_synthetic`.
* `scripts/build-host.ps1 [-Game default_tu3.xex] [-SdkInstall DIR] [-InstallSdk]` builds the native host
  `ttt2.exe` from `recomp/` against the ReXGlue SDK installed as a CMake package (the SDK's own consumption
  model: `cmake --build ... --target install`, then `find_package(rexglue)` through `CMAKE_PREFIX_PATH`);
  codegen runs inside the build, after `recomp/tools/find_switch_tables.py` has written
  `recomp/game/ttt2_switch_tables.toml` for the image (the manifests include it; the file is git-ignored with the
  image). The same route is exercised on Linux by `sdk-validation.yml` and on Windows by
  `rexglue-windows.yml` (`build_host` input), with the license-free kernel-import program standing in for the
  game image, so the player's build path has CI evidence without any game data.

## Recompiler tooling (`recomp/tools`, `scripts`)

* `recomp/tools/resolve_functions.py codegen --rexglue R --manifest M --image X --out F [--reference T]` runs
  `rexglue codegen` and turns each `UnresolvedCall` ("target not in any function") into a `[functions]` entry in F,
  then reruns; `verify` does one round on an existing log, `boot` does the same for the runtime's
  "Call to invalid or unregistered function" lines in host logs. An entry is written only when the branch decodes
  from the image and the reference T (TTT2 Web's table; only when ReXGlue's helper detection in the same codegen log
  matches its fingerprint) or the image's `.pdata` agrees: a reference function starting at the target gives the
  entry point alone, branch and target inside one reference function give that function with its size; anything
  else is reported and nothing is guessed. F carries the image's SHA-1 and is reset for another image. Tests:
  `tests/python/test_resolve_functions.py`; end to end on `recomp/tests/ppc/tailcall_ttt2.s` (three shapes, with
  `tailcall_ttt2.functions` as its reference via `recomp/tests/ppc/functions_to_toml.py`) in
  `scripts/codegen-smoke-test.sh` (codegen errors, resolution, harness run with the runtime trap, boot resolution,
  clean run) and on the real host in both SDK workflows (`host-boot-test.sh --expect-fatal`, then `--no-imports`).

* `recomp/tools/codegen_report.py <generated dir> [--log codegen.log]` - summarises a `rexglue codegen` run
  (functions, imports, jump tables, unimplemented sites, analysis errors, instruction mix) as REPORT.md / JSON
  with counts and addresses only; `--expect-*` options turn it into a check. Unit tests: `tests/python`
  (`python3 -I -m unittest discover -s tests/python`).
* `recomp/tools/find_switch_tables.py <image.xex> --toml tables.toml` - finds switch jump tables by symbolic
  execution and writes ReXGlue `[[switch_tables]]` entries; include the file from the codegen config. Needed
  because SDK 0.10.0's own detector misses offset tables and picks wrong registers (`docs/RESEARCH.md` 8a).
* `scripts/check_instruction_coverage.py [--expect-missing 2]` - compares `recomp/ttt2_instructions.txt` with
  the SDK's instruction builders.
* `recomp/tools/import_ttt2web_functions.py <TTT2.toml> --out recomp/ttt2_functions.ttt2web.toml` - converts
  TTT2 Web's (CC0) explicit function boundaries into ReXGlue's `[functions]` table; `recomp/ttt2_manifest.ttt2web.toml`
  includes the result and is only for images whose helper addresses match TTT2 Web's published set
  (`ttt2-xexinfo --helpers`). Unit-tested in `tests/python`.
* `scripts/make-ppc-test-xex.sh <source.c|.s> <out.xex>` - builds a license-free PowerPC test program and packs
  it as an XEX2 image (assembly through the SDK's bundled binutils, the Cygwin-built `.exe` variants on Windows);
  writes `<out.xex>.syms`. C sources need a Clang with the PowerPC backend (exit 3 without one): the official LLVM
  builds for Windows ship X86/ARM/AArch64 only, so the C programs are Linux-only and the assembly programs are the
  ones that run everywhere. A `<source>.imports` file next to the source (`library: slot_symbol, thunk_symbol, ...`)
  adds a real XEX2 import table (`IMPORT_LIBRARIES` header) so the program calls kernel exports like the game does:
  `recomp/tests/ppc/imports_ttt2.s` is the example (slots in `.rodata`, 16-byte thunks at the end of `.text`).
* `recomp/tools/triage_boot_log.py <log file | logs dir> [--md out.md] [--json out.json]` - reads one host log
  session (rotated parts in order) and reports the outcome (clean exit, crash, stall, closed, incomplete), the
  facts (image, backend, roots, imports), the timeline, the HOST CRASH block with the fault address translated to a
  guest address and region, stall dumps, the kernel stubs reached with TTT2 Web's (CC0) reference implementation
  for each (`recomp/tools/ttt2web_kernel_functions.txt`, 135 names), unimplemented variable imports, the distinct
  errors, and a numbered action list. Unit-tested on synthetic logs shaped like the real ones.
* `scripts/host-boot-test.sh <ttt2[.exe]> <game root> [timeout] [--expect-crash | --expect-stall]` - boots the
  real game host on the license-free kernel-import image (`recomp/tests/ppc/imports_ttt2.s` packed as
  `recomp/game/default.xex`) without GPU emulation (`--gpu_plugin=none`; on a display-less Linux it starts an
  Xvfb of its own) and checks its log: image identified, module launched, both kernel overrides reached, program
  returned, host exited with code 0. With `--expect-crash` (image: `recomp/tests/ppc/crash_ttt2.s`, a program
  that writes to the null page) it expects exit code 3 and a HOST CRASH block instead. With `--expect-stall`
  (image: `recomp/tests/ppc/stall_ttt2.s`, a program that spins forever) it runs with a 1 s watchdog period until
  the "possible stall" dump appears, then closes the window the way a player does (SIGTERM, which SDL turns into a
  close request; `taskkill` on Windows) and expects the per-thread dump, the kernel stub report the host writes on
  close, and exit code 0. Both workflows run all three forms, rebuilding the host on each image. `--gpu` lets the
  host load the staged Xenos plugin instead of running without GPU emulation (expects "graphics backend xenos" and
  no load or presentation-setup failure); the Windows workflow runs it as a non-blocking, informative step on the
  runner's software D3D12 adapter. On a Linux box without a Vulkan device the plugin loads but the instance
  creation fails ("ErrorIncompatibleDriver"), which the form reports.
* `scripts/run-host.ps1 [-GameRoot work\game] [-Exe ttt2.exe] [-Work work] [-TimeoutSeconds N] [-NoGpu]` - the
  owner's boot step as one command: runs the host on the game data root, waits for its exit (with a timeout it
  sends the close request a player's X button sends: `taskkill`, SIGTERM on Linux, and kills a host that ignores
  it for 20 s), runs the triage tool on the log session, copies the log to `<work>\boot-logs\` and writes
  `boot-triage.md`/`.json` and `boot-report.txt` (how the host ended, exit code, duration, outcome); its exit code
  is the host's. `owner-pipeline.ps1 -Boot [-BootTimeout N]` chains it after the build and the extraction. Both
  workflows run it under pwsh on the kernel-import image (clean exit) and on the stall image (timeout close: the
  triage reports the stall and the close).
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
tools. `sdk-validation.yml` (weekly/manual) builds the SDK, runs the codegen checks, the native run of the
switch-table program through the headless host (`--exec`), and builds the game host against the installed SDK
package; `rexglue-windows.yml` (weekly/manual) builds `rexglue.exe` on Windows and, with the `native_run` and
`build_host` inputs, also builds the headless host and runs the program there, and builds `ttt2.exe` the way a
player does (slow: both build the whole runtime).

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

The repository's own scripts fetch the pinned commit (`third_party/rexglue-sdk/PIN`): `scripts/fetch-rexglue-sdk.sh`
and `scripts/fetch-rexglue-sdk.ps1`. To evaluate another SDK commit without touching the pin, set
`REXGLUE_SDK_COMMIT=<sha>` (or pass `-Commit <sha>` to the PowerShell script), or dispatch `sdk-validation.yml` /
`rexglue-windows.yml` with their `sdk_commit` input, which runs the whole license-free suite on that snapshot (and,
on Windows with `build_host`, produces its package artifact). `docs/decisions/0002-sdk-pin-and-snapshot-evaluation.md`
holds the evaluation checklist and the rules for moving the pin.

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
also build `recomp/tests/host` and run the switch-table and kernel-import programs natively (slow: it builds the
runtime). The harness compiles `recomp/src/kernel_overrides.cpp` in, so `imports_ttt2` proves that a call through
the import table reaches the host's override of a runtime stub (`XamShowFriendsUI` -> 0,
`XamUserCreateStatsEnumerator` -> 1317) and the runtime's own `RtlCompareMemory` (5), with no stub called. On Windows
(Git Bash) add `--allow-no-ppc-clang`: the C cases are skipped there, the assembly cases and `--exec` run, and the
syntax check of the generated code against the SDK headers is Linux-only (the host build compiles it instead). To
compile-check the real host (`recomp/`, `Ttt2App`) on Linux, put any test image at `recomp/game/default.xex` and
configure `recomp/` with `-DREXSDK_DIR=third_party/rexglue-sdk/src` (Clang 20, `-mssse3`).

## Windows build of the codegen tool

`scripts\build-rexglue-windows.ps1` follows the SDK's own CI (LLVM clang + Ninja, `win-amd64` preset; needs
Visual Studio 2022's C++ workload for the Windows SDK headers). `.github/workflows/rexglue-windows.yml` runs the
same steps on a Windows runner on demand and uploads `rexglue.exe` as an artifact, so the owner can also download
the tool instead of building it. With the `build_host` input the run also installs the SDK as a CMake package,
moves the package away from its install location, adds the SDK's licence, every vendored component's licence file
and the source commit under `share/licenses/`, uploads it as `rexglue-sdk-install-win-amd64` (90 days) and builds
`ttt2.exe` from the moved copy (`build-host.ps1 -SdkInstall`), which proves the package works from any folder: the
owner unzips it and passes `-SdkInstall <dir>` to the pipeline instead of building the SDK (30-60 minutes). On
Linux the same relocation was verified by hand (package copied, host built and booted from the copy).

Without a Windows machine the PowerShell scripts are checked with the PowerShell 7 parser
(`pwsh -Command '[System.Management.Automation.Language.Parser]::ParseFile(...)'`) and the workflow files with
`actionlint`; both are cheap and worth running before a push, actionlint caught an invalid YAML step name once.
