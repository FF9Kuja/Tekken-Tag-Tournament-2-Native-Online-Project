# Status log

Newest first. Each entry says what was done, what was verified and where, and what is blocked.

## 2026-10-19 - Session 5: PR #6 merged, the owner's boot step as one command

**Completed**
* **PR #6 merged into `main`** (merge commit `5acfb19`): `host-boot-test.sh --gpu` and the Windows workflow's
  informative GPU step, which passed on the runner's software D3D12 adapter (run 39227512735; see session 4).
  The Windows workflow was dispatched on `main` afterwards (run 39230244565).
* **The owner's boot step as one command: `scripts/run-host.ps1`** (Windows PowerShell 5.1 / PowerShell 7; pwsh
  on Linux for CI). It checks `ttt2.exe`, the game data root and Python 3, starts the host from its own folder
  with `--game_data_root`, waits for its exit, and with `-TimeoutSeconds N` sends a host still running after N
  seconds the close request the X button sends (`taskkill`; SIGTERM on Linux), killing it 20 s later if it
  ignores that. Then it runs the triage tool on the log session the run wrote, copies the log to
  `<work>\boot-logs\`, writes `boot-triage.md`/`.json` and a `boot-report.txt` (how the host ended, exit code,
  duration, outcome line, the files to send), and exits with the host's exit code. A host that never opened its
  log is reported with the tails of its stdout/stderr instead. `owner-pipeline.ps1 -Boot [-BootTimeout N]`
  chains it after the build and the extraction and copies the triage into the report; the checklist's step 6 is
  now the pipeline command plus closing the window when it freezes.
* Triage outcome order: a stall dump followed by the window close is reported as the stall ("...; the window was
  closed afterwards") rather than as a plain close, since that is the owner's most likely first-boot shape; unit
  test added (18 Python tests).
* Both workflows run the boot step (`pwsh`) on the kernel-import image (host ends by itself, triage "clean exit")
  and on the stall image (1 s watchdog, 8 s timeout closes the window, triage "stall ...; closed afterwards");
  the CI dry run of the pipeline passes `-Boot` without a host and must report the skip.
* **Prebuilt SDK package for the owner.** The Windows workflow's `build_host` stage now moves the installed SDK
  package away from where CMake installed it, adds the SDK's BSD-3 licence, every vendored component's licence
  file and the source commit under `share/licenses/`, uploads it as the `rexglue-sdk-install-win-amd64` artifact
  (90 days) and builds `ttt2.exe` from the moved copy (`build-host.ps1 -SdkInstall`), which is the proof that
  the owner can unzip it anywhere. `owner-pipeline.ps1 -SdkInstall <dir>` passes it through and takes
  `rexglue.exe` from its `bin\`; `build-host.ps1` resolves the package path before configuring inside `recomp\`
  (a relative `-SdkInstall` broke there). The checklist's step 6 now starts with the download instead of the
  30-60 minute SDK build (`docs/DEPENDENCIES.md` records the artifact's contents and licences).

**Verified (where)**
* Linux container, PowerShell 7.4.6 under Xvfb, the real host built against the installed SDK package:
  kernel-import image -> "Ended: by itself; exit code 0; 0.2 s", log copied, triage "clean exit", script exit 0;
  stall image with `TTT2_WATCHDOG_MS=1000 -TimeoutSeconds 8` -> "still running after 8 s; sending the close
  request", "Ended: after the close request (8 s timeout); exit code 0; 8.2 s", the log holds the stall dump, the
  stub report at window close and "Window closing", triage "stall: ...; the window was closed afterwards";
  without a display the script reported "none written to .../logs" with the SDL error from stdout. 18 Python
  unit tests OK; hygiene check clean.
* Relocation of the installed package, Linux: `out/sdk-install` copied to another folder, `rexglue codegen` from
  the copy, `recomp/` configured with `CMAKE_PREFIX_PATH` on the copy, host built (runtime and GPU plugin staged
  from the copy) and booted on the kernel-import image: `host-boot-test: PASS`. The package's CMake files hold
  no absolute paths.
* The CI, SDK validation and Windows runs of this change are recorded in PR #7 (pending when this was written).

**Not verified / open**
* `run-host.ps1` on Windows PowerShell 5.1 and the `taskkill` close through `Start-Process`: the Windows workflow's
  `build_host` stage is the first run of the script on Windows (the boot test's `taskkill` close is verified there).
* The Windows package artifact (size, the host build from the moved copy, the licence bundle) comes with the
  Windows run of PR #7; a package built on the runner's LLVM consumed by an owner's different LLVM 20+ minor
  version is expected to link (same MSVC ABI) but is unverified until the owner's run.
* Everything on license-free programs; the real image still waits for the owner's run.

**Blocked / pending**
* Owner's files for Milestone 3 steps 3-5 and the first boot (Milestone 4): the owner path is now the
  `rexglue-sdk-install-win-amd64` download plus
  `scripts\owner-pipeline.ps1 -Iso ... -TitleUpdate ... -Codegen -BuildHost -SdkInstall <dir> -ExtractGame -Boot`
  and the three files it names (`work\boot-triage.md`, the log copy, `work\boot-report.txt`) plus `work\report.txt`.

**Next task (resumable state)**
* PR #7 (this boot step): CI, SDK validation and Windows `build_host` runs green, merge, restart the branch, Windows
  run on `main`. Then iterate on the owner's first-boot report as described in session 4's next task.

## 2026-10-19 - Session 4: PR #2 merged with CI evidence, player-side host build path

**Completed**
* **PR #3 merged into `main`** (merge commit `3a90395`, 13 commits) with CI 10/10, the SDK validation run and the
  Windows run green on its head: on a Windows runner `rexglue.exe` built, the TTT2-shaped assembly programs
  were assembled with the SDK's Cygwin binutils, **recompiled code executed natively on Windows** for the first
  time (switch-table program 12 of 12 values with the finder's tables; the kernel-import program reached the host
  overrides), the SDK was installed as a package and **`ttt2.exe` was built against it**.
* **First boot of the real host binary** (`ttt2`, the ReXApp with window and runtime, built from the installed
  SDK), on Linux under Xvfb with the license-free images. Findings, all fixed and now tested:
  1. An unhandled guest fault never ends the process: the SDK's POSIX signal handler returns when no handler
     claims a fault, the faulting instruction re-executes, and the switch-table program (recompiled on the
     SDK's own tables, which are wrong for several shapes) produced 695,000 "Unhandled guest access violation"
     lines in 15 seconds and rotated the log away. `Ttt2App::OnHostException` now writes the HOST CRASH report
     and exits with code 3 (`TTT2_CRASH_KEEP_RUNNING=1` for a debugger). Measured: the same program now ends in
     0.2 s with the report in the log (`scripts/host-boot-test.sh --expect-crash`).
  2. The SDK's `gpu_plugin` cvar defaults to "" (no GPU emulation, `VdXxx` calls ignored) and the plugin is only
     loaded from the executable's folder, where nothing staged it: the owner's first boot would have run blind.
     `recomp/CMakeLists.txt` stages `rexgpu-xenos` next to the host (`GPU_PLUGINS xenos`; the runtime library
     too on Linux) and `OnPreSetup` selects it unless `--gpu_plugin=<name>` or `--gpu_plugin=none` was given
     (an empty value cannot be passed: CLI11 takes the next argument as the value).
  3. After a clean guest exit the process hung in the SDK's teardown: gdb showed `InputSystem::Shutdown` ->
     `~MnkInputDriver` -> `CallInUIThreadSynchronous` on the UI thread after its loop ended. The host now ends
     the process from `OnGuestThreadExit` after the stub report, as the SDK's own window-close path does.
  With those, `scripts/host-boot-test.sh` boots the host on the kernel-import image: image identified, module
  launched, `XamShowFriendsUI` and `XamUserCreateStatsEnumerator` reached the host overrides, program returned,
  exit code 0 in 0.3 s. Both workflows now run it (Linux under Xvfb; Windows on the runner's desktop) and upload
  the boot log. SDL's dummy video driver is not compiled into the SDK's SDL3 ("dummy not available"), so Xvfb it is.
* **Two gaps in the owner's path to the first real boot, closed**: (1) nothing ran the switch-table finder on
  the real image before codegen, although the SDK's own detector is measured to get several of TTT2's switch
  shapes wrong (a wrong table is a crash at the first such switch) - `build-host.ps1`, the owner pipeline and the
  Linux CI host step now run `recomp/tools/find_switch_tables.py` on `recomp/game/default.xex` and both manifests
  include its `game/ttt2_switch_tables.toml` (git-ignored with the image; codegen fails clearly when it is
  missing: "included file not found"); (2) the title update's own files were never extracted - `ttt2-stfs
  extract-all <package> <dir>` (ctest on the synthetic package) and the pipeline's `-ExtractGame` now write them
  to `work\update`, which the host mounts as `update:` next to `work\game`. Verified on Linux: finder -> codegen
  with the include -> host build -> boot test pass; the owner checklist explains both.
* **Boot-log triage tool** (`recomp/tools/triage_boot_log.py`): reads one host log session (rotated parts in
  order) and writes the action list for it: outcome (clean exit / crash / stall / closed / incomplete), facts,
  timeline, the HOST CRASH block with the fault address translated through the logged arena bases to a guest
  address and region, stall dumps, the kernel stubs reached with TTT2 Web's (CC0) reference implementation for
  each (`recomp/tools/ttt2web_kernel_functions.txt`: the 135 names with file:line), unimplemented variable imports,
  distinct errors. Unit-tested on synthetic logs shaped like today's real ones; the owner checklist runs it on the
  first-boot log and sends its report with the log.
* **PR #2 merged into `main`** (merge commit `8dff15e`, 16 commits) after every check passed on its head: Linux
  clang and gcc, Windows MSVC and clang-cl, hygiene + Python tools (CI), and the SDK validation workflow on a
  GitHub runner: SDK built with Clang 20, 1458 PPC instruction test cases / 5725 assertions, all codegen smoke
  cases, and the **native run of the switch-table program through the headless host** (fails with the SDK's own
  tables, passes with the finder's, no kernel stubs reached). Two CI fixes were needed: `lld` was not installed
  in the Linux CI jobs (`ld.lld: not found`), and the smoke test let the packer fall back to the runner's
  unversioned clang 18; the packer now accepts `$LD`, `ld.lld`, `ld.lld-<major>` or the clang driver's own lld,
  and the smoke test hands it its compiler.
* **Player-side host build path** (`scripts/build-host.ps1`): the SDK installed as a CMake package
  (`cmake --build ... --target install`, the SDK's own distribution model) and `recomp/` configured with
  `CMAKE_PREFIX_PATH`; `rexglue codegen` runs inside the build, so no generated file is touched by hand.
  Verified on Linux here with the license-free switch-table program as the game image: `find_package(rexglue)`
  finds the package, codegen runs, `ttt2` links (11 build steps). `sdk-validation.yml` repeats exactly that
  sequence in CI; `rexglue-windows.yml` gains a `build_host` input that runs it on Windows.
* **`ttt2-iso extract-all <image> <dir>`**: whole-disc extraction (sanitised names, streamed files), the game
  data root the host runs from (`ttt2.exe --game_data_root <dir>`); end-to-end ctest on the synthetic image
  (3 files in 1 directory, byte-identical `default.xex`).
* **Owner pipeline**: `-BuildHost` (+ `-InstallSdk`) builds `ttt2.exe` from the patched executable, `-ExtractGame`
  extracts the disc into `work\game`; `docs/OWNER_CHECKLIST.md` step 6 describes the first boot attempt and
  what to send (the console log with the kernel stub report; never the game folder).
* **First-boot diagnostics in the host**: a last-chance crash reporter (`rex::arch::ExceptionHandler`, installed
  after the runtime's MMIO handler) logs the fault kind, host PC, fault address, the guest thread and the most
  recent kernel stub calls, flushes the log and lets the normal crash path continue; the watchdog logs a
  per-thread dump (state, priority, suspend count, wait result) when the thread census and the stub total stay
  unchanged for three ticks while the main thread runs. The stub counter keeps a ring of the last 16 calls. The
  owner checklist now points at the host's `logs\` folder (the SDK writes `logs/` next to the executable).
* **TTT2 Web's public-domain addresses, gated by a layout fingerprint**: TTT2 Web (CC0) publishes the eight
  save/restore helper addresses of its author's executable and 18,351 explicit function boundaries generated
  on it (leaf functions without `.pdata`, functions outgrowing their recorded size). The boundaries are
  converted to ReXGlue's `[functions]` form (`recomp/tools/import_ttt2web_functions.py` ->
  `recomp/ttt2_functions.ttt2web.toml`, addresses and sizes only). Because the executable version TTT2 Web used
  is not stated, the helper addresses serve as fingerprint: `ttt2-xexinfo --helpers` and `ttt2-validate` print
  "Layout vs TTT2 Web: N of 8 helper addresses match", and only a full match makes the boundaries applicable;
  `recomp/ttt2_manifest.ttt2web.toml` includes them, the default manifest does not, and the owner pipeline runs
  the variant codegen as a second pass only on a match (both reports are written for comparison).
* **Predicted kernel coverage for TTT2** (`docs/RESEARCH.md` 8a): TTT2 Web, playable natively on its own CC0
  runtime, implements 135 kernel/XAM functions; ReXGlue 0.10.0 implements 130 of them and stubs 5 (XAM UI /
  stats calls). The runtime-export generator now counts `REX_HOOK_RAW` hooks (11, e.g. `DbgPrint`) as
  implemented, which it had classified as missing.
* **First kernel overrides in the host** (`recomp/src/kernel_overrides.cpp`): the five XAM calls above
  (`XamShowFriendsUI`, `XamShowGamerCardUIForXUID`, `XamShowMarketplaceUI`, `XamShowMarketplaceDownloadItemsUI`
  -> `ERROR_SUCCESS`, `XamUserCreateStatsEnumerator` -> `ERROR_NO_SUCH_USER`, TTT2 Web's results) defined as
  `__imp__` symbols in the host, which generated code binds to at link time ahead of the runtime library's stubs;
  each logs its first call. This is the mechanism for every stub the owner's report shows the game reaching.
* **The override mechanism executed natively** (`recomp/tests/ppc/imports_ttt2.s`): the packer
  (`scripts/ppc_elf_to_xex.py --imports`) now writes a real XEX2 `IMPORT_LIBRARIES` header from a `<source>.imports`
  declaration, so a license-free program calls kernel exports exactly like the game: 4-byte slots, 16-byte thunks at
  the end of `.text`, `bl thunk`. `rexglue codegen` resolved all three ordinals (xam.xex 703 and 759, xboxkrnl.exe
  282) to `__imp__` calls and treated the thunk range as non-code, as it does for the game. The headless harness
  now compiles `kernel_overrides.cpp` in (as the game host does) and the native run returned `0`, `1317`, `5`
  (two overrides, then the runtime's own `RtlCompareMemory`), with the overrides' first-call log lines and zero
  stub calls. Negative control, run once here with the override source removed from the harness: the same program
  logged `__imp__XamShowFriendsUI STUB` and `__imp__XamUserCreateStatsEnumerator STUB`, returned r3 untouched
  (`0x5A5A5A5A`, then `0`), the stub counter reported 2 calls to 2 stubs and the harness failed - which is what the
  game would see for any stub it reaches. The smoke test runs the program as its fifth case, natively under
  `--exec`. Verified on Linux here; the SDK validation workflow repeats it on every run.
* The first Windows run that reached the smoke test (ReXGlue codegen tool workflow, run 3) found two more
  Windows-only problems, fixed here and awaiting the next Windows run: the official LLVM Windows build has no
  PowerPC backend (`clang -print-targets` lists no `ppc32`; "No available targets are compatible with triple
  powerpc-unknown-linux-gnu"), so the two C test programs cannot be built there - the packer now exits 3 for
  that case and the smoke test skips the C cases on request (`--allow-no-ppc-clang`, which the Windows workflow
  passes; Linux CI keeps covering them); and the scripts tested for the SDK's Linux ELF binutils, which Git Bash
  does not consider executable, so the TTT2-shaped assembly cases were skipped - they now pick the SDK's
  Cygwin-built `powerpc-none-elf-*.exe` on Windows. Also: `python` instead of `python3` on the Windows runners,
  LF-only symbol maps so `awk` finds the result address, and the Linux-only syntax check is skipped on Windows
  (the `--exec` host build compiles the generated code for real there).
* Two findings from the first CI runs of the host-build route, fixed and reproduced locally: on a fresh tree the
  SDK's generated CMake glue only picks the recompiled sources up when `generated/default` exists at configure
  time (the link failed with an undefined `PPCImageConfig`), so `build-host.ps1` and the SDK workflow run
  `rexglue codegen` before the first configure; and the smoke test's positional-argument re-split dropped the
  empty SDK argument the Windows workflow passes, shifting the work directory into the SDK slot.
* Windows workflow fixes from its first run on `main`: the SDK checkout failed with "Filename too long"
  (vulkan-loader fuzzer test data beyond 260 characters) - `fetch-rexglue-sdk.ps1` now clones with
  `core.longpaths=true`; the LLVM install falls back to the latest package when the pinned version is missing;
  `rexglue.exe` is uploaded even when the smoke test fails.

**Verified (where)**
* GitHub Actions on PR #2's head `8881b74`: 10/10 CI check runs green; SDK validation run 39209513350 green.
* PR #3 (merged as `3a90395`): CI 10/10 on `138a4f6`; SDK validation run 39216946123 (five smoke cases incl.
  the native runs of the switch-table and kernel-import programs, SDK package install, host build); Windows run
  39216950997 (`rexglue.exe`, assembly cases with the SDK's Cygwin binutils, **native execution on Windows**,
  `ttt2.exe` built against the installed SDK). The Windows workflow also ran green on `main` after the merge
  (run 39218860764).
* PR #4 branch: CI green on every head; SDK validation run 39219293142 (`d064da8`) green with the host boot
  test under Xvfb; Windows run 39219295783 (`d064da8`, `build_host`): **`ttt2.exe` booted on the Windows
  runner** on the kernel-import image and passed every boot-test check (`rexgpu-xenos.dll` staged).
* Linux container: 52 doctest cases / 612 assertions, 15 ctest entries (new: `stfs_extract_all_synthetic`),
  17 Python unit tests, Clang 18 and GCC 13 `-Werror`; the real host booted, crashed on purpose and exited on
  purpose as described above; finder -> codegen -> host -> boot verified after the manifest change.

**Not verified / open**
* The graphics path: `host-boot-test.sh --gpu` lets the host load the staged Xenos plugin. Here (no Vulkan
  device) the plugin loads and the SDK then fails to create a Vulkan instance ("ErrorIncompatibleDriver",
  "Graphics presentation setup failed"), after which the app exits without launching the module - the expected
  outcome on a GPU-less box, and the first evidence that the plugin file, its exports and the load path work. The
  Windows workflow runs the form as a non-blocking step; on the runner's software D3D12 adapter it **passed**
  (PR #6, run 39227512735: `rexgpu-xenos.dll` loaded, "graphics backend xenos", the D3D12 presentation was set
  up, the module ran, both overrides were reached, exit 0) - the first exercise of the whole graphics path,
  windows and swap chain included, though nothing was drawn (the test program draws nothing). The GPU path on a
  real adapter with the real game is still the owner's first boot.
* The stall detector fired natively on a license-free program that spins forever (`recomp/tests/ppc/stall_ttt2.s`,
  `host-boot-test.sh --expect-stall`: "possible stall ... unchanged for 3000 ms" with four per-thread lines,
  1 s watchdog period); the crash reporter fired on the illegal-instruction case and on `crash_ttt2.s` (a write
  to the null page; HOST CRASH block with the fault address, exit 3 in 0.2 s). Both workflows run the normal,
  crash and stall forms of the boot test, rebuilding the host on each image. The stall form also closes the
  window the way a player does (a quit signal / `taskkill`) and found that the SDK's close path hard-exits
  without `OnShutdown`, so the full kernel stub report was lost exactly when the owner closes a frozen window;
  `Ttt2App::OnWindowCloseRequested` now writes it on the way out (verified: report, "Window closing", exit 0,
  7 s for the whole test). The blocking boot tests run with `--gpu_plugin=none`; the `--gpu` form above is
  the only run with graphics so far, on a software adapter and a program that draws nothing.
* `find_switch_tables.py` is verified on the synthetic switch program only; its result on the real image (the
  count and the codegen report) comes with the owner's first run.
* PR #4 merged as `4478a34` (CI 10/10, SDK validation and Windows runs green on its head, PowerShell 5.1 dry run
  included), PR #5 (stub report on window close, stall test through the real close path) as `e7f4084`; the Windows
  workflow ran green on `main` after each merge (runs 39224566733, 39227258710).
* Everything on license-free programs; the real image still waits for the owner's run.

**Blocked / pending**
* Owner's files for Milestone 3 steps 3-5 and the first boot attempt (Milestone 4): the whole owner path is now
  one command (`scripts\owner-pipeline.ps1 ... -Codegen -BuildHost -InstallSdk -ExtractGame`, checklist step 6),
  followed by `recomp\out\build\win-amd64-release\ttt2.exe --game_data_root work\game` and the triage tool
  on the log.

**Next task (resumable state)**
* PR #6 (`--gpu` boot-test form, informative Windows GPU step) is green and ready to merge; after the merge
  restart the branch from `main` and iterate on the owner's first-boot report: implement each stub the triage
  report lists as a host override (TTT2 Web's reference is given per stub), fix what the HOST CRASH block or the
  stall dump points at, repeat. Until the report arrives, the preparatory work left is small: both workflows
  already run the normal, crash, stall and (Windows, informative) GPU forms of the boot test.

## 2026-10-19 - Session 3: recompiler measured on TTT2-shaped code, first native execution, validation tools

**Completed**
* **Toolchain in the container**: LLVM 20.1.8 from the GitHub release tarball (apt.llvm.org is blocked by the
  session proxy), the SDK's Linux dependencies, the pinned ReXGlue SDK built (`rexglue` 0.10.0.2-dev, 809 steps,
  about 6 minutes), the runtime library built twice more for the host tests below.
* **Jump tables, measured** (`recomp/tests/ppc/switch_ttt2.s`, nine dispatch shapes including TTT2's
  `lis, rlwinm, addi, lwzx` order, `bgtlr` bounds checks, `mr` aliases, recomputed indices, byte- and
  halfword-offset tables, `ori` low halves, a leaf without `.pdata`, both `lwzx` operand orders):
  - SDK 0.10.0 recognises 7 of 9 dispatches; it misses every offset-table form (its `add` operand heuristic
    tracks the base register) in its own documented order too (`probe_offset.s` experiment, 0 of 4).
  - Several of the 7 are emitted with the **wrong switch register** (the register that holds the loaded target,
    or `r12`), and the table length comes from an entry-validity scan, not the bounds check.
  - **Proof by execution**: the recompiled program run natively through the ReXGlue runtime (headless harness,
    `recomp/tests/host`) traps and never finishes with the SDK's tables; with the tables from our own finder it
    produces all 12 expected values, 3 runs out of 3 (`scripts/codegen-smoke-test.sh --exec`).
* **`recomp/tools/find_switch_tables.py`**: clean-room symbolic finder (approach after TTT2 Web, CC0) that emits
  ReXGlue `[[switch_tables]]` entries; finds 9/9 on the test image, 4/4 on the offset probe, 0 false positives on
  the C programs; handles the compare-on-a-copy and scaled-in-place index cases (stretched label lists) and
  reports unresolved dispatches with the reason.
* **Instruction coverage, measured** (`scripts/check_instruction_coverage.py`, `recomp/ttt2_instructions.txt`):
  64 of the 66 instructions TTT2 Web added for TTT2 have builders in SDK 0.10.0; `macchwu`/`maclhwu` have none and
  the SDK's decoder reads those opcode-4 words as `vsldoi128` (they occur only in data-as-code in TTT2). All 64
  translate from `recomp/tests/ppc/insn_ttt2.s` (verified per mnemonic in the generated code).
* **Diagnostics**: `recomp/tools/codegen_report.py` (functions, imports, switches, unimplemented sites,
  analysis errors, instruction mix; REPORT.md + JSON; unit-tested on synthetic output, `tests/python`);
  `scripts/codegen-smoke-test.sh` rebuilt around it (two-pass switch case, instruction case, `--exec`).
* **Game-file validation**: `ttt2::xex::check_patch_compatibility` (title IDs, versions, delta-descriptor
  windows, digest notes) wired into `ttt2-xexpatch` (refuses mismatched pairs, `--force` overrides);
  `ttt2-validate <iso> <tu-or-folder>` one-command report (finds the TU3 package in a folder, applies the update
  in memory when built with Clang, hashes, imports, helper addresses, `--json`, `--work`);
  `ttt2-xexinfo --imports` (ordinal names from the kernel/XAM export tables, `src/xex/xbox_exports_table.inc`
  generated from the pinned XenonRecomp checkout) and `--helpers` (the eight save/restore helpers by byte pattern).
  Known-hash table gains the reported TU3 image SHA-1.
* **Host**: `recomp/src/ttt2_app.{h,cpp}` implements the `ReXApp` hooks (paths, backend log, executable
  identification against title `4E4D0859` / version 0.0.3.2, import summary, guest-thread watchdog);
  `recomp/tests/host/headless_main.cpp` is the headless runtime harness.
* **Kernel stub counter** (`recomp/src/stub_counter.{h,cpp}`, `stub_name.hpp`): a spdlog sink on the runtime's
  loggers that counts every `<name> STUB` warning the SDK's stubbed exports emit; `Ttt2App` reports the top
  entries on each watchdog tick and the complete list (most-called first, first-seen order) when the main
  thread exits and at shutdown, so the first boot of the real game yields the exact list of system calls the
  runtime must implement. The headless harness compiles the same code and prints the stubs a test program
  reached (`--max-stub-calls N` makes more a failure). The line parser is unit-tested in the main project.
* **Windows**: `scripts/build-rexglue-windows.ps1` (the SDK's own CI recipe: LLVM clang + Ninja, `win-amd64`
  preset), `.github/workflows/rexglue-windows.yml` (builds `rexglue.exe` on a Windows runner, runs the smoke
  test, uploads the tool), `scripts/owner-pipeline.ps1` (one command from ISO + TU to a text report, optional
  codegen + report). `sdk-validation.yml` installs Clang 20 through `llvm.sh` and runs the new checks.
* `recomp/ttt2_config.toml`: the `[[invalid_instructions]]` entry copied from XenonRecomp semantics removed
  (in ReXGlue `data` is an address, consulted only when following branches).
* **End-to-end tool test on synthetic inputs** (`tests/tools/make_validate_fixtures.cpp`, `run_expect.cmake`):
  a generator writes a disc image whose `default.xex` is a synthetic PE title executable, a matching title-update
  package (its patch's source digest is the SHA-1 of the base's headers), a mismatching package and a
  console-style `Content` tree with decoys; four ctest entries run `ttt2-validate` the way the owner does and
  check exit codes, the printed reasons (digest note, import counts, helper addresses, "version mismatch", "no
  Title Update 3 package found") and the files written to `--work`. New `--no-patch` option keeps the runs
  identical with and without the Clang-only patcher.
* **Runtime import coverage**: `scripts/gen_runtime_exports.py` reads the SDK's export declarations into
  `src/xex/rexglue_runtime_exports.inc` (SDK 0.10.0: 471 implemented functions, 2406 logging stubs, 24 mapped
  variables); `ttt2-xexinfo --imports` and `ttt2-validate` now mark every import of an image as implemented /
  stub / missing in the pinned runtime and list the stubbed and missing names; `owner-pipeline.ps1` adds the scan
  to the report (step 5b). The owner's report will therefore show TTT2's runtime gaps before anything is run.
* **Windows files checked without Windows**: all five PowerShell scripts parse with PowerShell 7.4.6 (parser API
  only, not executed); `actionlint` 1.7.7 found that `sdk-validation.yml` was invalid YAML (an unquoted `:` in a
  step name) - fixed, all workflows lint clean.

**Verified (where)**
* Linux container, Clang 18 and GCC 13 `-Werror`: 51 doctest cases / 604 assertions, 13 ctest entries (4 of them
  end-to-end runs of `ttt2-validate` on synthetic fixtures); Python unit tests 6/6; hygiene check clean;
  `actionlint` clean; PowerShell parse clean.
* `scripts/codegen-smoke-test.sh --exec` with the SDK built here: smoke 6/0/0, jumptable 2/0/0, switch_ttt2
  12 functions / 7 SDK switches / 9 with the finder, insn_ttt2 8 functions with 64 mnemonics translated; the
  native run passes with the finder's tables and fails with the SDK's (a trap or a guest thread that never
  finishes, depending on the run); all generated units compile against the SDK headers. The harness's stub
  counter reports `kernel stubs: 0 calls to 0 stubs` for the test program, which the script now asserts.
* Everything above on license-free programs only. Nothing was run on game files (none here, by design).

**Not verified / open**
* Windows: the ReXApp host (`recomp/`, `Ttt2App`) builds and links on Linux against the SDK with a license-free
  program as its game (verified here); it was not run (no display in the container). The Windows build of the
  host and of `rexglue.exe` are exercised only by the GitHub workflows (run on demand).
* `scripts/owner-pipeline.ps1` and `scripts/build-rexglue-windows.ps1` parse (PowerShell 7.4.6) but were not
  executed here (they need Visual Studio); their first real run is the owner's or the Windows workflow's.
* Which `lwzx`/`add` operand orders TTT2's compiler emits is unknown until the owner's codegen report; the finder
  handles both, the SDK one.
* The real TU3 image: counts of unresolved calls, jump tables (526-535 expected) and data-as-code sites come
  from the owner's run (`docs/OWNER_CHECKLIST.md`, step 5).

**Blocked / pending**
* Owner's files for Milestone 3 steps 3-5 (hashes, codegen report).
* At the end of the session `git push` and the GitHub API both answered 403 for this repository ("Claude doesn't
  have GitHub access ... for your organization" / "Resource not accessible by integration"): the Claude GitHub App
  had lost access. The branch `claude/ttt2-native-execution-vw1vio` was kept locally and handed to the owner as a
  git bundle; re-linking the app lets the push and the pull request go through.

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
