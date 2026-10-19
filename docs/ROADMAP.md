# Roadmap

Milestones are sequential; each has exit criteria that must be demonstrated, not claimed. "Needs Windows" marks
steps that cannot run in the Linux development container. Status legend: done / in progress / planned / blocked.

## Milestone 1 - Technical research  (done, 2026-10-18)

* [x] Reference repositories inspected (Tekken 6 NO + source, tekken6-recomp, TTT2Recomp, TTT2 Web).
* [x] Recompilation frameworks evaluated; XenonRecomp/XenonAnalyse built on Linux; ReXGlue SDK fetched and built
      as far as the container allows.
* [x] TTT2 landscape, challenges, architecture, assumptions, questions and risks documented.
* Deliverables: `docs/RESEARCH.md`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `docs/DEPENDENCIES.md`.

## Milestone 2 - Project foundation  (done, 2026-10-18)

* [x] Repository structure, `.gitignore`, content policy (`docs/LEGAL.md`), hygiene check in CI and as a git hook.
* [x] CMake 3.25 project with presets for Linux (clang, gcc) and Windows (MSVC, clang-cl).
* [x] Native C++ libraries `ttt2_core` (endian, bounds-checked reader, SHA-1/SHA-256, FNV-1a) and `ttt2_xex`
      (XEX2 header parser, known-title data) and the `ttt2-xexinfo` tool.
* [x] Unit tests (doctest): 22 cases / 311 assertions, passing with Clang 18 and GCC 13 under `-Werror`.
* [x] Reproducible scripts (`scripts/build.sh`, `build.ps1`, `setup-ubuntu.sh`).
* [x] GitHub Actions: hygiene + Linux clang/gcc + Windows MSVC/clang-cl (Windows jobs validated only when the
      workflow first runs on GitHub - see `docs/STATUS.md`).
* Exit criterion met on Linux; Windows CI result pending the first push.

## Milestone 3 - Recompilation foundation  (in progress)

Goal: a verified codegen workflow for the TTT2 TU3 image, with limitations documented.

1. [x] ReXGlue SDK pinned (`third_party/rexglue-sdk/PIN`, fetch + Linux build scripts; the `recomp/` skeleton
   from `rexglue init` is in the tree). [x] Windows build recipe for the `rexglue` tool
   (`scripts/build-rexglue-windows.ps1`, proven by `.github/workflows/rexglue-windows.yml` on demand).
2. License-free validation in CI: [x] (a) the SDK's PPC instruction tests through
   `scripts/sdk-recompile-tests.sh` (1458 cases pass on Linux); [x] (b) `scripts/ppc_elf_to_xex.py` +
   `scripts/codegen-smoke-test.sh`: Clang-built PowerPC programs wrapped as XEX2/PE images with `.pdata`,
   recompiled end to end with expected function counts and syntax-checked output; [x] (c) instruction coverage
   for the 66 instructions TTT2 Web lists: `scripts/check_instruction_coverage.py` (64 have builders;
   `macchwu`/`maclhwu` decode as VMX128 and matter only in data-as-code) and `recomp/tests/ppc/insn_ttt2.s`
   translated end to end; [x] (d) jump tables: `recomp/tests/ppc/switch_ttt2.s` (9 TTT2-shaped dispatches) and
   `recomp/tools/find_switch_tables.py` producing `[[switch_tables]]`, proven by native execution
   (`--exec`); [x] (e) codegen reports: `recomp/tools/codegen_report.py`; [x] (f) end-to-end runs of
   `ttt2-validate` on synthetic disc/title-update fixtures in ctest (matching pair, folder search, mismatch, no
   update).
3. [x] Ingestion tools (session 2): XDVDFS reader + `ttt2-iso`, STFS reader + `ttt2-stfs`, XEX decrypt /
   basic decompress + `ttt2-xexinfo --image-hash`, XEXP patching via `ttt2-xexpatch` (XenonUtils, MIT, Clang-only).
   [x] (session 3) `ttt2-validate` one-command check, TU-vs-base pre-check, `--imports` / `--helpers` scans,
   `scripts/owner-pipeline.ps1`. [ ] Run them on the owner's copy (`docs/OWNER_CHECKLIST.md`) and record the
   verified hashes in `known_titles.hpp` (**needs the owner's files; run locally, report hashes only**).
4. [ ] Run ReXGlue codegen on the real TU3 image (**owner's PC or WSL; outputs stay local**): the report tool
   gives unresolved calls, missing instructions, jump tables found; run the finder on the same image and compare
   with the 526-535 tables the other ports report; add its `[[switch_tables]]` and the data-as-code
   `[[invalid_instructions]]` to `recomp/ttt2_config.toml`; cross-check with XenonAnalyse.
5. [ ] Document compatible executable versions (base 0.0.0.2 + TU3 0.0.3.2) and the refusal rules for others
   (the tools already refuse other titles/versions; the owner's hashes complete the table).
6. [x] Diagnostic pipeline: codegen logs, validation reports (`codegen_report.py`, `ttt2-validate --json`,
   `owner-pipeline.ps1` report). [ ] `recomp/REPORT.md` of the real image.

Exit: codegen of the TU3 image completes with zero validation errors (or every remaining error explained), and the
generated code compiles (**needs Windows for the host link**; a Linux compile-only check of the generated TUs is
possible).

**Measured so far (SDK 0.10.0, license-free inputs, session 3):** the SDK's own jump-table detector misses all
byte/halfword-offset tables and switches on the wrong register for several absolute shapes; manual
`[[switch_tables]]` entries from our finder are therefore mandatory for TTT2 and are the path the pipeline uses.

## Milestone 4 - Native runtime  (started)

1. [x] `recomp/src/ttt2_app.{h,cpp}` (`Ttt2App` on the SDK; compile-checked on Linux against the SDK with a
   license-free program as the game). [x] Player-side build path: `scripts/build-host.ps1` builds `ttt2.exe`
   against the SDK installed as a CMake package (verified on Linux here; `sdk-validation.yml` repeats it in CI,
   `rexglue-windows.yml build_host` on Windows). [ ] First Windows CI run of that job green.
2. [x] First boot diagnostics: executable identification, import summary, guest-thread watchdog; the headless
   harness `recomp/tests/host` runs recompiled programs through the runtime without a window.
   [x] import coverage: every import of the owner's image is classified against the pinned runtime
   (implemented / stub / missing; `ttt2-xexinfo --imports`, `ttt2-validate`, `scripts/gen_runtime_exports.py`);
   the SDK's stubs log `<name> STUB` at warn level, which is the import trace. [x] stub-call counter in
   `Ttt2App` and the headless harness (`recomp/src/stub_counter.*`: which stubbed system calls the game reaches,
   how often, logged on watchdog ticks and in full at exit). [x] stall detector (per-thread dump when the census
   and the stub total stay unchanged for three watchdog ticks) and a host crash report (fault kind, host PC,
   fault address, guest thread, most recent stub calls) - compile-checked against the SDK, exercised only by a
   real boot.
3. Iterate on boot failures with the owner's test reports (**needs Windows + GPU + game files**): threads, waits,
   vblank/DPC timing, XAM UI/content calls, TLS, file I/O through the SDK VFS (`game:`, `update:`).
4. Graphics: first frames through the D3D12 backend; audio: XMA playback; input: pads + keyboard.
5. Measurable milestone: boot to the title screen; then to the main menu.

## Milestone 5 - Offline gameplay

Character select, Practice, Arcade, local Versus; fix rendering/audio/timing issues; stable 60 fps; input delay
option (original 2-frame FIFO on/off, as TTT2Recomp offers); save data; resolution options. Exit: reliably playable
offline, verified by the owner's test matrix.

## Milestone 6 - Rollback netcode

Determinism tooling first (input record/replay, per-frame state hashes, snapshot self-test), then frame/input/seed
hooks, snapshots with dirty-page tracking, audio boundary, GGPO integration, local two-instance sync test with
simulated latency, desync detection. Exit: verified rollback matches over LAN/loopback with 0-8 frames of
rollback and no desyncs over long sessions.

## Milestone 7 - Multiplayer infrastructure

Private matches by code, lobbies, matchmaking, connection diagnostics, UPnP/hole punching, version pairing,
abuse prevention basics. Exit: public test with external players.

## Milestone 8 - Launcher and release

Windows launcher with validation, local build pipeline (bundled compiler), settings, updates, diagnostics,
packaging; documentation for players. Exit: a distributable package containing no game content.

## Owner involvement checkpoints

* M3 step 3-4: run the ingestion/codegen tools on your own copy (instructions will be provided, copy-paste ready)
  and send back only the report files (hashes, counts, logs) - never the game files.
* M4 onward: Windows test runs with logs; a PC with a DirectX 12 GPU.
* Decisions: repository license (M2 follow-up), launcher UI toolkit (M8), hosting for services (M7).
