# Status log

Newest first. Each entry says what was done, what was verified and where, and what is blocked.

## 2026-10-18 - Session 1: Milestones 1 and 2

**Completed**
* Research of all reference projects and toolchains; `docs/RESEARCH.md`, `docs/ARCHITECTURE.md`,
  `docs/ROADMAP.md`, `docs/DEPENDENCIES.md`, `docs/LEGAL.md`, ADR 0001.
* Project foundation: CMake project + presets, `ttt2_core`, `ttt2_xex`, `ttt2-xexinfo`, doctest tests, scripts,
  hygiene check, CI workflow.

**Verified (where)**
* Linux container (Ubuntu 24.04): Clang 18 and GCC 13 builds with `-Werror`; `ctest`: 3/3 tests (16 doctest cases,
  274 assertions) pass; `ttt2-xexinfo` parses a synthetic XEX2 image and a synthetic `.xexp`; the hygiene checker
  flags a synthetic XEX and passes the tree.
* XenonRecomp + XenonAnalyse (MIT, commit `ddd128b`) build on Linux with Clang 18.
* Clang 18 emits 32-bit big-endian PowerPC ELF (`--target=powerpc-unknown-linux-gnu`, LLD `elf32ppc`).
* ReXGlue SDK v0.10.0: submodules fetched (709 MB); codegen tool build attempted - see "Blocked / pending".

**Not verified**
* Windows builds (no Windows toolchain in the container): the MSVC and clang-cl CI jobs run on the first push.
* Any behaviour on real game files: none are available here, by design.
* Hashes in `known_titles.hpp` are reported by other projects, not reproduced.

**Blocked / pending**
* ReXGlue `rexglue` tool build on Linux: configure needed GTK3, Xss and XCB dev packages (installed); the build
  result is recorded below when finished. If it fails, Milestone 3 starts by fixing the Linux build or using the
  SDK's published Linux binaries.
* Repository license: owner decision (MIT recommended, `docs/LEGAL.md`).
* Everything past Milestone 2 needs the owner's Windows PC and game files for validation.

**Known issues / technical debt**
* `ttt2::Expected` is a stand-in for `std::expected` (C++23) - replace when the toolchain floor allows.
* `ttt2-xexinfo` JSON output is hand-written; fine for now, but a JSON writer helper will be needed once the
  launcher consumes tool output.
* `TTT2_WARNINGS_AS_ERRORS` is not yet enabled for the Windows CI jobs.
