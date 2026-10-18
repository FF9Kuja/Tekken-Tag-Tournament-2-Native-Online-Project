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

## Tools

* `ttt2-xexinfo <file.xex|.xexp> [--json] [--headers] [--no-hash]` - identify an Xbox 360 executable or title
  update without decrypting it. It prints the title ID and version, hashes the file and tells you whether the
  hashes match a known copy. Example on the owner's disc copy (do not send the file, just the output):

  ```
  ttt2-xexinfo "D:\TTT2\Default.xex"
  ttt2-xexinfo "D:\TTT2\TU_174Q22P_0000008000000.00000000000O2" --headers   # (after STFS extraction: Milestone 3)
  ```

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

`.github/workflows/ci.yml`: hygiene check; Linux clang and gcc (`-Werror`); Windows MSVC (VS 2022 generator) and
clang-cl (Ninja). All jobs build the tools and run the tests; Linux jobs also smoke-test `ttt2-xexinfo`.

## Working with the ReXGlue SDK on Linux (Milestone 3 preview)

What was needed in this session to configure the SDK on Ubuntu 24.04 (beyond the compilers): `libgtk-3-dev`,
`libxss-dev` (or `-DSDL_X11_XSCRNSAVER=OFF`), `libx11-xcb-dev` and the XCB/Xinput/Xrandr dev packages, plus the
usual SDL3 audio/Wayland dev packages and `nasm` for FFmpeg. The codegen-only target is `rexglue`:

```sh
git clone --recursive https://github.com/rexglue/rexglue-sdk   # pin the commit from docs/DEPENDENCIES.md
cmake -S rexglue-sdk -B build/sdk -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build build/sdk --target rexglue --parallel
```
