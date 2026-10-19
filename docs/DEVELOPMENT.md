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

* `ttt2-iso info|list|identify|extract <image.iso> ...` - reads an Xbox 360 disc image in place (XGD3/XGD2/XGD1
  or an extracted partition); `identify` finds `default.xex`, parses and hashes it (`--image-hash` also hashes
  the decrypted image); `extract` copies one file out.
* `ttt2-stfs info|identify|extract <package> ...` - reads a title-update package (LIVE/PIRS/CON); `identify`
  reports the contained `.xexp`'s source -> target version.
* `ttt2-xexpatch <base.xex> <patch.xexp> <out.xex>` - applies a title update (Clang builds only; needs
  `scripts/fetch-xenonrecomp.sh` or `.ps1` first).
* The owner's end-to-end instructions are in `docs/OWNER_CHECKLIST.md`.

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

Measured in the first session on Ubuntu 24.04 (the SDK wiki is right: **Clang 20** is needed on Linux - Clang 18
reports `__cpp_concepts 201907L`, so libstdc++ 13 hides `std::expected`, which `rex/result.h` uses). The SDK's
core also needs SSSE3 enabled (`-mssse3`; the tekken6-recomp README passes the same flag).

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
