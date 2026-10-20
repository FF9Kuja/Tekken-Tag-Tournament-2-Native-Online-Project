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
   `scripts/owner-pipeline.ps1`. [x] (2026-10-19) The owner ran `ttt2-iso identify` on their disc: title
   `4E4D0859`, media ID `5D762D06`, version 0.0.0.2, retail AES-128-CBC, basic compression, SHA-1 match with the
   published retail hash (recorded in `known_titles.hpp`). [x] (2026-10-19) The owner's first full run reached
   the import analysis (8 of 8 helper addresses match TTT2 Web) and failed at the recompilation because the
   pipeline handed the encrypted, compressed executable to the finder; fixed with the preparation step
   (`ttt2-xexinfo --write-plain`, pipeline step 5c, stop-on-critical, retail-form fixtures and regressions,
   the SDK workflows running the whole chain on a retail-form wrap of the license-free program). [ ] The Title
   Update 3 steps wait for the update (the owner does not have it; `docs/OWNER_CHECKLIST.md` gives the
   legitimate route); the pipeline's base-image mode runs every later step on the retail executable meanwhile.
4. [ ] Run ReXGlue codegen on the real image (**owner's PC or WSL; outputs stay local**; the base executable
   0.0.0.2 now, the TU3 image when the update is available): the report tool
   gives unresolved calls, missing instructions, jump tables found; run the finder on the same image and compare
   with the 526-535 tables the other ports report; add its `[[switch_tables]]` and the data-as-code
   `[[invalid_instructions]]` to `recomp/ttt2_config.toml`; cross-check with XenonAnalyse. [x] TTT2 Web's 18k
   explicit function boundaries are converted and ready (`recomp/ttt2_functions.ttt2web.toml`), applied only if
   the owner's image matches TTT2 Web's helper-address fingerprint (the pipeline checks and runs both variants).
   [x] (2026-10-19) First codegen on the owner's base image: 596 switch tables, 49,708 functions sealed, stopped
   on 14 `UnresolvedCall` errors (a ReXGlue analysis limitation: `b` targets are never registered, gap fill misses
   code after a function's last branch; `docs/RESEARCH.md` 8b). [x] `recomp/tools/resolve_functions.py` resolves
   such errors, and the runtime's calls to unregistered functions from a boot log, with entries verified against
   the image and TTT2 Web's table; the pipeline, `build-host.ps1` and the SDK workflows run codegen through it.
   [x] The switch-table finder follows the dispatch code exactly (more shapes, a complete register-write model,
   other paths into a dispatch checked; `docs/RESEARCH.md` 8c) and `recomp/tools/audit_switches.py` (pipeline step
   6c) checks every `bctr` the codegen emitted, naming the SDK detector's certain miscompiles and listing the
   dispatches still unread with their code. [ ] The owner's rerun: codegen complete, the audit's numbers, then the
   host build and the first boot (Milestone 4).
5. [ ] Document compatible executable versions (base 0.0.0.2 + TU3 0.0.3.2) and the refusal rules for others
   (the tools already refuse other titles/versions; the owner's hashes complete the table).
6. [x] Diagnostic pipeline: codegen logs, validation reports (`codegen_report.py`, `ttt2-validate --json`,
   `owner-pipeline.ps1` report). [ ] `recomp/REPORT.md` of the real image.

Exit: codegen of the TU3 image completes with zero validation errors (or every remaining error explained), and the
generated code compiles (**needs Windows for the host link**; a Linux compile-only check of the generated TUs is
possible).

**Measured so far (SDK 0.10.0, license-free inputs, sessions 3 and 6):** the SDK's own jump-table detector misses all
byte/halfword-offset tables and switches on the wrong register for several absolute shapes (on `switch_ttt2.s`, 6
of the 7 switches it makes are certain miscompiles); manual `[[switch_tables]]` entries from our finder are
therefore mandatory for TTT2 and are the path the pipeline uses.

## Milestone 4 - Native runtime  (started)

1. [x] `recomp/src/ttt2_app.{h,cpp}` (`Ttt2App` on the SDK; compile-checked on Linux against the SDK with a
   license-free program as the game). [x] Player-side build path: `scripts/build-host.ps1` builds `ttt2.exe`
   against the SDK installed as a CMake package (verified on Linux here; `sdk-validation.yml` repeats it in CI,
   `rexglue-windows.yml build_host` on Windows). [x] First Windows CI run of that job green (`ttt2.exe` built
   against the installed SDK on a Windows runner; recompiled test programs executed natively on Windows).
   [x] The real host boots a license-free image headless (Linux under Xvfb, Windows runner): module launched,
   kernel overrides reached, clean exit (`scripts/host-boot-test.sh`); the crash path exits with a report.
   [x] The owner's boot step as one command (`scripts/run-host.ps1`, `owner-pipeline.ps1 -Boot`): run, close on
   timeout, triage, report; both workflows run it on the kernel-import and the stall images. [x] Prebuilt SDK
   package artifact (`rexglue-sdk-install-win-amd64`, licences included) so the owner skips the SDK build; the
   Windows workflow builds and boots the host from the relocated package (`-SdkInstall`).
2. [x] First boot diagnostics: executable identification, import summary, guest-thread watchdog; the headless
   harness `recomp/tests/host` runs recompiled programs through the runtime without a window.
   [x] import coverage: every import of the owner's image is classified against the pinned runtime
   (implemented / stub / missing; `ttt2-xexinfo --imports`, `ttt2-validate`, `scripts/gen_runtime_exports.py`);
   the SDK's stubs log `<name> STUB` at warn level, which is the import trace. [x] stub-call counter in
   `Ttt2App` and the headless harness (`recomp/src/stub_counter.*`: which stubbed system calls the game reaches,
   how often, logged on watchdog ticks and in full at exit). Prediction from TTT2 Web's 135-function runtime:
   130 implemented in ReXGlue, 5 XAM UI/stats stubs (`docs/RESEARCH.md` 8a); those five are overridden in the
   host already (`recomp/src/kernel_overrides.cpp`, TTT2 Web's results), the mechanism for the rest, and the
   mechanism is executed natively by the kernel-import test program (`recomp/tests/ppc/imports_ttt2.s`, in the
   smoke test's `--exec` run). [x] stall detector (per-thread dump when the census
   and the stub total stay unchanged for three watchdog ticks) and a host crash report (fault kind, host PC,
   fault address, guest thread, most recent stub calls) - compile-checked against the SDK, exercised only by a
   real boot.
3. Iterate on boot failures with the owner's test reports (**needs Windows + GPU + game files**): threads, waits,
   vblank/DPC timing, XAM UI/content calls, TLS, file I/O through the SDK VFS (`game:`, `update:`).
   [x] The whole game compiles and runs natively on the owner's PC (2026-10-20); each boot so far ended on a call
   to a function only a pointer reaches. [x] Those are registered ahead of time from pointers in the image's data
   (`resolve_functions.py codegen --indirect`), and the pipeline's boot loop repairs the rest by itself with stop
   conditions (no progress, repeated trap, run limit, time, disk space, failed rebuild; `docs/RESEARCH.md` 8d).
   [x] With those in place the game starts 28 threads and stays on a black window; its one error is the open of the
   bare disc device `\Device\Cdrom0`, which the runtime does not provide (`docs/RESEARCH.md` 8e). The host answers
   the disc sector query as unsupported, as TTT2 Web does, so the loader reads files by name. [x] Boot monitor: file
   access, GPU/audio/input/dialog milestones, frames presented, and per-thread waits (objects, owners, call sites)
   and guest call stacks in every stall dump and at window close; the triage reports deadlocks and wait chains.
   [x] The owner's second run (PR #16 host): 10,760 frames presented, all black; 2 file reads in 180 s; the bare
   device opened is `\Device\Image` (no sector query at all); one thread with 339.5 million kernel calls. The runtime
   never advances `KeTimeStampBundle`'s 64-bit clocks, which TTT2 reads from memory: the host now refreshes them
   (`docs/RESEARCH.md` 8f). Missing names are answered as the console and both reference ports answer them;
   `\Device\Image` stays unmapped (neither port maps it). [x] Frame monitor (draws, resolves, clears per frame;
   optional pixel check), busy-thread flags, critical sections whose count grows between dumps.
   [ ] Confirm on the owner's copy: the loader reads its data, the spinning thread calms down, frames get draws.
4. Graphics: first frames through the D3D12 backend; audio: XMA playback; input: pads + keyboard.
5. Measurable milestone: boot to the title screen; then to the main menu.

## Milestone 5 - Offline gameplay

Character select, Practice, Arcade, local Versus; fix rendering/audio/timing issues; stable 60 fps; input delay
option (original 2-frame FIFO on/off, as TTT2Recomp offers); save data; resolution options. Exit: reliably playable
offline, verified by the owner's test matrix.

Input (requirements and design: `docs/INPUT.md`, ADR 0003; native execution in Milestones 3-4 keeps priority):
1. [x] `recomp/src/input/` core: binding parser, TOML profiles (default `tekken8-numpad`: WASD, Numpad 4/5/1/2 for
   1/2/3/4, Numpad 6 = 1+2, Numpad 3 = 3+4, Tag = Numpad 8, B = Start, V = Back), opposite-direction policies
   (neutral default), press latch, macro keys, layout translation, `PlayerInput` word, latency statistics; 12
   doctest cases (136 assertions) in the first-party CI on Linux and Windows, compiled into the host as well.
   Not yet wired to the SDK: the game cannot use it until step 2.
2. [ ] `KeyboardPadDriver` + device assignment on the SDK's `InputDriver` interface, installed by the host's
   `input_factory` next to the SDL3/XInput pad drivers; license-free guest program polling `XamInputGetState` in the
   headless harness with injected key events (CI).
3. [ ] In-game Controls dialog (ImGui): press-to-bind, profiles (save/duplicate/reset/switch), direction target,
   opposite-input policy, player slot; `--input_profile`; input latency statistics (`--input_stats`).
4. [ ] Owner verification on the real game (`docs/INPUT.md` 6): menus, character select, Practice, matches from the
   keyboard alone; confirm the Tag pad button and the direction target; pad + keyboard on player 1; keyboard as
   player 2. Keep the game's original input behaviour; no Tekken 8 mechanics.

## Milestone 6 - Rollback netcode

Determinism tooling first (input record/replay, per-frame state hashes, snapshot self-test), then frame/input/seed
hooks, snapshots with dirty-page tracking, audio boundary, GGPO integration, local two-instance sync test with
simulated latency, desync detection. Exit: verified rollback matches over LAN/loopback with 0-8 frames of
rollback and no desyncs over long sessions. Inputs enter as per-frame `PlayerInput` words (button mask + 8-way
direction) built after the keyboard cleaning and latching of `docs/INPUT.md`, so keyboard and pad players are
indistinguishable to the netcode and to replays.

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
