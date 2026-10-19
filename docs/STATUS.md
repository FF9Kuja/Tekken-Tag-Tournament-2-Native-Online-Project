# Status log

Newest first. Each entry says what was done, what was verified and where, and what is blocked.

## 2026-10-19 - Session 2: synchronisation attempt and Milestone 3 pipeline tools

**Completed**
* Milestone 3 ingestion pipeline (all clean-room, synthetic-fixture tested, Clang 18 + GCC 13 `-Werror`):
  `ttt2-iso` (XDVDFS disc images: info/list/identify/extract), `ttt2-stfs` (title-update packages:
  info/identify/extract), XEX basefile decryption (AES-128 from FIPS-197, session key from the retail key) and
  basic decompression, `ttt2-xexinfo --image-hash`, and `ttt2-xexpatch` (title-update delta patches via the
  pinned MIT XenonUtils patcher; Clang-only).
* ReXGlue SDK pinned (v0.10.0) with fetch/build scripts (sh + PowerShell); XenonRecomp pinned likewise.
* `docs/OWNER_CHECKLIST.md`: the exact commands for the owner's files; CI extended; a weekly/manual SDK
  validation workflow added.

**Verified (where)**
* **`rexglue codegen` runs end to end on license-free programs**: `scripts/ppc_elf_to_xex.py` wraps a
  Clang-built PowerPC ELF into a minimal XEX2/PE32 image with a `.pdata` exception directory (the SDK's
  loader checks the `MZ\x90\x00` DOS dword, PowerPC-BE machine type, the Xbox subsystem and 64 KiB page
  descriptors); all seven analysis phases pass, every function is recompiled with no unimplemented or error
  markers, and the generated translation units compile (syntax check) against the SDK headers with Clang 20.
  `scripts/codegen-smoke-test.sh` repeats this for `recomp/tests/ppc/*.c`; the SDK validation workflow runs it.
* Linux container: 42 doctest cases / 473 assertions, 7 ctest entries, both compilers.
* STFS layout cross-check: an independent reader known to work on the real TTT2 title updates reads this
  project's synthetic packages and recovers every file byte for byte.
* `ttt2-xexpatch` builds from the pinned sources and rejects non-patch inputs; it could not be run on a real
  `.xexp` (no title update here, by design).

**Blocked / pending**
* **Push still blocked** after the owner's permission update: git push through the session proxy -> 403 (app
  installation check), GitHub API writes -> 403 ("not accessible by integration" / "write access not permitted
  through this proxy"). Reads work and the proxy's own token reports push permission on the repository, so the
  missing piece is the Claude GitHub App installation on this repository (or a fresh session after it is
  installed). All commits are local and in the bundle handed to the owner.
* Windows CI, the real-file pipeline run and the recompilation of the TU3 image wait on the push and on the
  owner's files respectively.

## 2026-10-18 - Session 1: Milestones 1 and 2

**Completed**
* Research of all reference projects and toolchains; `docs/RESEARCH.md`, `docs/ARCHITECTURE.md`,
  `docs/ROADMAP.md`, `docs/DEPENDENCIES.md`, `docs/LEGAL.md`, ADR 0001.
* Project foundation: CMake project + presets, `ttt2_core`, `ttt2_xex`, `ttt2-xexinfo`, doctest tests, scripts,
  hygiene check, CI workflow.

**Verified (where)**
* Linux container (Ubuntu 24.04): Clang 18 and GCC 13 builds with `-Werror`; `ctest`: 3/3 tests (22 doctest cases,
  311 assertions) pass; `ttt2-xexinfo` parses a synthetic XEX2 image and a synthetic `.xexp`; the hygiene checker
  flags a synthetic XEX and passes the tree.
* XenonRecomp + XenonAnalyse (MIT, commit `ddd128b`) build on Linux with Clang 18.
* Clang 18 emits 32-bit big-endian PowerPC ELF (`--target=powerpc-unknown-linux-gnu`, LLD `elf32ppc`).
* ReXGlue SDK v0.10.0: submodules fetched (709 MB); the `rexglue` codegen tool **builds on Linux with Clang 20**
  (809 build steps; see "ReXGlue build result").

**Not verified**
* Windows builds (no Windows toolchain in the container): the MSVC and clang-cl CI jobs run on the first push.
* Any behaviour on real game files: none are available here, by design.
* Hashes in `known_titles.hpp` are reported by other projects, not reproduced.

**Blocked / pending**
* **Pushing to GitHub is blocked**: both `git push` (through the session's proxy) and the GitHub API return 403
  because the Claude GitHub App is not installed on this repository. The commits exist locally and in a git bundle
  handed to the owner. Fix: install the app on the repository (https://github.com/apps/claude/installations/select_target)
  or reconnect GitHub (https://claude.ai/connect-github); then the branches are pushed and the pull request opened.
* ReXGlue `rexglue` tool build on Linux: three attempts, each fixing one requirement - (1) SDL3 needed the X11
  screensaver/XCB/GTK3 development packages; (2) the SDK core needs `-mssse3`; (3) `rex/result.h` uses
  `std::expected`, which libstdc++ 13 withholds from Clang 18 (`__cpp_concepts 201907L`), so Clang 20 is required
  as the wiki says. The fourth attempt (Clang 20) is recorded under "ReXGlue build result" below.
* Repository license: owner decision (MIT recommended, `docs/LEGAL.md`).
* Everything past Milestone 2 needs the owner's Windows PC and game files for validation.

**ReXGlue build result (2026-10-19)**
* `rexglue` v0.10.0 built on Ubuntu 24.04 with Clang 20.1.2, `-mssse3`, Tracy/perf counters off, SDL screensaver
  check off. Attempts with Clang 18 fail on `std::expected` (libstdc++ 13 + `__cpp_concepts 201907L`).
* `rexglue init --project-name ttt2 --xex-path ... --game-root ...` works against an image with valid XEX2
  headers (our synthetic test image); the generated skeleton is now `recomp/` in this repository.
* `rexglue codegen` on a **plain PowerPC ELF** (built here with Clang 18 `--target=powerpc`, linked at
  `0x82000000`): the ELF loads into the guest arena and the Register phase starts, then stops with
  "Exception DataDirectory not found (size=0)" - v0.10.0 requires a PE `.pdata` directory. License-free
  codegen smoke tests therefore need either a synthetic XEX2/PE wrapper with `.pdata` (Milestone 3 option) or
  the SDK's own route below.
* `rexglue recompile-tests` with the SDK's bundled `powerpc-none-elf` binutils (Linux binaries included) turns
  the SDK's PPC assembly tests into Catch2 cases: 3 files -> 63 test cases generated in 0.0 s. This is the
  license-free way to validate instruction translation; `scripts/sdk-recompile-tests.sh` reproduces it
  (166 files -> 1458 Catch2 cases in 0.2 s).
* **The SDK's full PowerPC instruction suite builds and passes on Linux**: `ppc_tests` (SDK configured with
  `-DREXGLUE_BUILD_TESTS=ON`, Clang 20): "All tests passed (5725 assertions in 1458 test cases)". This is the
  strongest evidence so far that ReXGlue's instruction translation is sound on this toolchain.

**Code review (2026-10-19)**
* A review pass over the branch (with ASan/UBSan builds) found and fixed: unbounded header tables and accepted
  duplicate keys (memory exhaustion on hostile files), optional-header offsets not confined to the header area,
  an unchecked page-descriptor copy, an NTSC-J region description bug, non-ASCII bytes breaking the JSON
  output, smoke tests that failed with `TTT2_BUILD_TOOLS=OFF`, the hygiene hook scanning the working copy
  instead of the index, and a hook installer that assumed `.git` is a directory. Regression tests added.

**Known issues / technical debt**
* `ttt2::Expected` is a stand-in for `std::expected` (C++23) - replace when the toolchain floor allows.
* `Sha1`/`Sha256` duplicate their block-buffering and padding code; a shared Merkle-Damgard helper would
  remove ~70 lines (noted by the review, deferred).
* `ttt2-xexinfo` JSON output is hand-written; fine for now, but a JSON writer helper will be needed once the
  launcher consumes tool output.
* `TTT2_WARNINGS_AS_ERRORS` is not yet enabled for the Windows CI jobs.
