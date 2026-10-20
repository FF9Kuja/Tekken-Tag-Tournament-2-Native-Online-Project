# Status log

Newest first. Each entry says what was done, what was verified and where, and what is blocked.

## 2026-10-20 - Session 8: the black screen - the one error explained, the sector query answered like TTT2 Web, a boot monitor

**The owner's report (2026-10-20, checkout `0d1d012`).** Native execution again, with everything before it working
(583 switch tables, 348 verified entries of which 327 ahead of time, codegen without unresolved calls, `ttt2.exe`, 78
files extracted). The game created 28 guest threads and reached no kernel stub; the window stayed black (no Bandai
Namco logo) until the pipeline closed it after about 180 s; the watchdog reported a possible stall after 15 s; one
error in the log, `ResolvePath(\Device) failed - device not found`; exit code 0 from the close request; the boot loop
stopped after one run (no unregistered function called). The report files were described, not attached.

**Completed**
* **The one error, explained and reproduced (`docs/RESEARCH.md` 8e).** The SDK's `OpenFile` resolves the parent of
  a path first, and nothing is mounted at `\Device`, so the error is exactly what an open of a bare device path
  (`\Device\<name>`) prints; ReXGlue has no disc device. `recomp/tests/ppc/io_ttt2.s` opens `\Device\Cdrom0` and the
  runtime prints the identical line.
* **The likely cause and the change.** ReXGlue answers the disc sector query (`FileSectorInformation`, class 26)
  with success and a made-up sector; TTT2 Web answers it as unsupported, noting that TTT2's loader then reads files
  by name instead of raw sectors through the disc device. The host now answers it the same way
  (`recomp/src/io_overrides.cpp`); `TTT2_SECTOR_QUERY=runtime` restores the runtime's answer. Not yet confirmed on
  the real game; the diagnostics below show either way.
* **Host wrappers of runtime exports (`recomp/src/runtime_exports.*`).** A host `__imp__` definition can call the
  runtime's own function (`dlsym(RTLD_NEXT)` / `GetProcAddress` over the modules other than the executable);
  `Ttt2App` checks every wrapper's original at start-up. 47 exports are wrapped; nothing else changes what they return.
* **Boot monitor (`recomp/src/boot_monitor.*`, `wait_overrides.cpp`, `milestone_overrides.cpp`).** File access
  (each path opened, each failure with status, thread and guest call site, the bare-device explanation, read
  failures, device controls); GPU set-up with arguments, the vblank counter and ring write pointer, frames presented;
  audio clients and frames; controller polls; system dialogs; thread creation with start addresses; an activity line
  every watchdog tick, whose progress counters now feed the stall detector. Every stall dump (15 s, again at 60 s)
  and a new dump at window close give each thread its blocking call, objects (event/semaphore state, the owner of a
  mutant or critical section), timeout, duration and call site, its traced calls, and its guest call stack read
  from its own stack; plus the busiest calls of the last tick and the last 12 file events. No locks or clock reads
  in the hot path; logs are bounded (64 paths per kind, 16 read failures, 8 calls per dialog).
* **Triage (`recomp/tools/triage_boot_log.py`).** Reads all of it: boot-progress table, file access, milestones,
  threads created, per-thread details of every dump; recommends from it (no frame presented and where the boot
  stopped; a bare device opened with or without the sector query answered; deadlock cycles and wait chains through
  lock owners; waits without a timeout on objects nobody signals). Old logs still parse.
* **Tests.** `recomp/tests/ppc/io_ttt2.s` (ten values checked in the harness, with and without
  `TTT2_SECTOR_QUERY=runtime`; the real host with `host-boot-test.sh --expect-io`) and `wait_ttt2.s` (a deadlock:
  `--expect-stall --expect-waits`, and the boot step's triage must report the wait chain); both added to the smoke
  test and to both workflows (Linux SDK validation and the Windows `build_host` job); 4 new triage tests.
* **Local runs (Linux, pinned SDK):** the smoke test with `--exec` (all checks, the new ones included); the real host
  on `imports_ttt2`, `io_ttt2` (`--expect-io`), `stall_ttt2`, `wait_ttt2` (`--expect-stall --expect-waits`) and
  `crash_ttt2`; `run-host.ps1` under PowerShell 7 on `wait_ttt2` (the triage reports the wait chain and the
  window-close dump); 92 Python tests; actionlint; hygiene.

* **CI and workflows on PR #16 (`55e35ec`):** CI green (push 39470736185, PR 39470791472); SDK validation on Linux
  39470797065: the smoke test with native execution (the new `io_ttt2` harness runs in both modes), the real host on
  the deadlock (`--expect-stall --expect-waits`, then the boot step's triage with the wait chain) and on the
  file-system program (`--expect-io`); the Windows workflow with `build_host` and `native_run` 39470801417: the same
  on Windows, every boot logging that the export wrappers found the runtime's originals (through `GetProcAddress`
  in `rexruntime.dll`), and the owner pipeline end to end under Windows PowerShell 5.1 unchanged.

**Not verified yet:** the owner's copy: whether the sector query was the cause, and what the boot monitor shows if
the window stays black.

## 2026-10-20 - Session 7: the boot repair runs by itself; functions only a pointer reaches registered ahead of time

**The owner's report (2026-10-20).** The latest real-game run compiled the whole game, built `ttt2.exe` and reached
native execution again. The boot repair verified three more calls to unregistered functions against TTT2 Web's table
and added them (21 saved entries; the last `0x82D6FC78`), then stopped at its three-run limit, so the owner had to
restart the pipeline every three rebuilds.

**Completed**
* **PRs #13 and #14 merged** (merge commits `0af48d7`, `1025d28`); on `main` afterwards CI 39330841504 and the
  Windows workflow 39330859742, green.
* **Investigation (`docs/RESEARCH.md` 8d).** On the owner's first codegen log, 695 of TTT2 Web's 18,351 function
  starts are functions ReXGlue never registered, `0x82D6FC78` and the 14 codegen-error targets among them; 94% are 32
  bytes or shorter (vtable methods: ReXGlue's VTableScanner only reads RTTI vtables, and the 337 it found are nearly
  all Havok's). TTT2 Web derived those starts from pointers in the image's data, so they are predictable from the
  image before a boot traps on them.
* **Ahead-of-time registration (`recomp/tools/resolve_functions.py codegen --indirect`).** After a successful codegen
  it registers every reference function ReXGlue did not register whose address a 4-aligned word in a data section
  holds, when the code checks pass (code at the address, no fall-through into it, not inside a `.pdata` function, no
  switch case label, an extent of plain code ending in a branch or return, nothing registered inside it); a start
  without a pointer is never registered this way. One batch, one more codegen round; entries codegen fails with are
  taken out and remembered, and failing that the batch is rolled back to the entries that compiled before. Every
  entry must then be registered in the generated code (the report names any that is not). `--drop-indirect`
  removes the ahead-of-time entries; the 21 verified entries and every other one are never touched.
* **The boot loop runs by itself (`scripts/owner-pipeline.ps1`).** Up to `-BootRounds` runs (default 10, capped at
  50); it stops on a run that ends some other way, no new verified entry, a repeated trap (an address the loop
  added, or one the host was built with), `-BootMinutes` (default 480, checked before each rebuild), less than
  `-MinFreeGb` (default 5) GB free, or a failed rebuild, and keeps every entry. The runs after a rebuild close the
  game after 300 s even with `-BootTimeout 0`. Step 9c summarises every run. It reads the triage and the resolver's
  summary lines instead of JSON (Windows PowerShell 5.1). `build-host.ps1 -SkipCodegen` stops each rebuild from
  running the finder and codegen twice; `-NoIndirect`, `-NoGpu` and `-Reference` pass through.
* **Tests.** 13 new Python tests (every ahead-of-time rule on a purpose-built image, the batch accepted, narrowed,
  rolled back and rolled back at the round limit, `--drop-indirect`, unregistered entries, the boot summary lines);
  `recomp/tests/ppc/pointer_ttt2.s` (two pointer-only shapes folded into the functions that branch to them, three
  negative controls) and `tailcall_ttt2.s` from scratch with `--indirect` in the smoke test, both run natively;
  `tests/powershell/test_boot_repair.ps1` (19 cases, the loop's own code with scripted boots; CI under PowerShell 7
  and 5.1); `tests/tools/make_test_iso.cpp` and the Windows workflow running the whole pipeline on a test disc.
* **Measured with the pinned SDK:** registering a function that ReXGlue had folded into another one splits it
  cleanly: an unconditional branch becomes a tail call, a conditional one a conditional tail call; `pointer_ttt2`
  computes `50 6 57 205 204 1004 62` with the two entries and traps at its first pointer call without them.
* **Local runs:** the smoke test with `--exec` (39 checks, 5 new), 88 Python tests, 26 ctests on both Linux presets
  with warnings as errors, the loop test under PowerShell 7.4 (19 cases; 4 mutations of the loop each caught),
  hygiene.
* **CI and workflows on the PR:** CI green on `8ecf61d` (push 39360683588, PR 39360688743: the loop test under
  PowerShell 7 and Windows PowerShell 5.1, the new ctests on Linux and Windows); SDK validation on Linux 39359070186
  (`eb9a439`; no code changed since): all 39 smoke checks with native execution; the Windows workflow with
  `build_host` and `native_run` 39360689124: the smoke test with native execution on Windows, part 4 of the
  boundary cycle (`tailcall_ttt2` from scratch with `--indirect`: no trap), and the owner pipeline end to end under
  Windows PowerShell 5.1 on a test disc carrying `pointer_ttt2`. With `-NoIndirect`, run 1 trapped at
  `cond_target`, run 2 at `over_target` after the loop's own rebuild, and run 3 ended cleanly (3 runs, 2 automatic
  rebuilds); by default, step 6 registered both functions and the first run ended cleanly. The first Windows run
  (39359073181) passed every report check of the `-NoIndirect` run and then failed its last one: a case-insensitive
  `CRITICAL` match that the triage's `critical` log level satisfied; the check now looks for the pipeline's own
  `CRITICAL:` line.

**Not verified yet:** the owner's image: how many of the 695 carry a pointer in data, whether codegen accepts the
batch, and how far the next boots get.

## 2026-10-19 - Session 6: PR #12 merged; switch dispatches made exact before the owner's rerun

**Completed**
* **PR #12 merged into `main`** (merge commit `4814479`): the function-boundary resolver
  (`recomp/tools/resolve_functions.py`), the runtime trap handling (exit code 4, triage, pipeline step 9b) and the
  child-PowerShell fix. Verified on the PR head `198186a`: CI (runs 39317087036, 39317138117), SDK validation on
  Linux (39317093509: smoke test with native execution, the real host's trap / resolve / rebuild / clean-exit cycle)
  and the Windows workflow with `build_host` (39317098464: the same cycle with `ttt2.exe`, `run-host.ps1` and the
  triage). On `main` afterwards: CI 39319511810 and the Windows workflow with `build_host` 39319532916, green.
* **Switch dispatches (this PR).** The owner's run left 18 dispatches the finder could not read, and any `bctr`
  without our table goes to ReXGlue's own detector: on `switch_ttt2.s` 6 of the 7 switches it makes are certain
  miscompiles (they test the loaded target; every dispatch traps), and a `bctr` it does not recognise becomes an
  indirect call that traps on a case label. The finder (`recomp/tools/find_switch_tables.py`) now symbolically
  executes the code before each `bctr` exactly: a complete model of which registers each instruction writes (the
  previous one missed `rld*` and every update-form load/store), the bounds branch's direction (`bge` gives max
  entries, `ble`/`blt` give none), new shapes (table address before the bounds check, bias folded into the table,
  `clrlslwi`/`clrldi`/`sldi`/`rldic`/`mulli`, `add` + D-form load, signed halfword offsets, two-level tables,
  mask-bounded tables), and other paths into a dispatch (branch targets, table labels, function starts) checked: one
  before the bounds check restarts the analysis, one after it must read the same entry, or the site is reported.
  Unread dispatches get their reason and disassembled code in the JSON. `recomp/tools/audit_switches.py` (pipeline
  step 6c, `build-host.ps1`) rebuilds each instruction address in the generated code and checks every `bctr`: the
  finder's tables as written, the SDK detector's switches with a verdict (certain miscompile / suspect / consistent /
  unverified), the unread dispatches with their code (`work\switch-audit.md`). The boot resolver names an unread
  dispatch in the function a trap lands in (step 9b passes the finder's report).
* **Verification (license-free inputs, `docs/RESEARCH.md` 8c):** `tests/python/test_find_switch_tables.py` runs
  every reported table in a small PowerPC interpreter for every index and every alternate entry path (encoders
  checked against GNU as; named shapes, negative cases, 300 seeded random dispatches in CI); locally 172,500 random
  dispatches during development, the final version's last 50,000 with no failure; against the previous finder on 10,000: nothing it
  found correctly is lost, 242 of its tables were wrong on another path, 6,875 new ones found. `switch_ttt2.s`: the
  9 tables are identical to the previous finder's. New `recomp/tests/ppc/switch_shapes.s` through the real codegen:
  9 tables read and emitted as written, S10 (another path with another table) reported, the native run computes
  `13 24 32 44 51 62 71 80 83 92 0`; the audit: 0 mapping problems, 6 certain miscompiles in the SDK-only
  `switch_ttt2` build. Local runs: the smoke test with `--exec` (all checks), 75 Python tests, 24 ctests, hygiene.

* **The first full host build, prepared (follow-up PR).** The real game is about 300 generated files of up to 1 MB;
  `build-host.ps1 -Jobs <n>` / `owner-pipeline.ps1 -BuildJobs <n>` limit the parallel compiles for a PC whose
  memory runs short, the report shows the drive's free space before the build and how long codegen and the build
  took, a failed build's output is scanned for out-of-memory and disk-full messages (a `HINT:` line says what to
  change), and step 6c reads the audit's miscompile count from its summary line instead of parsing its JSON (Windows
  PowerShell 5.1's `ConvertFrom-Json` has trouble with very large inputs). The Windows workflow builds with
  `-Jobs 2` and checks the timing line.

**Not verified yet:** the owner's executable. The rerun will show how many of the 18 the finder now reads, how
ReXGlue emitted the other `bctr` sites (the audit's verdicts), and then whether codegen completes with the resolver
(PR #12), the host builds and the first boot.

## 2026-10-19 - Session 5: PR #6 merged, the owner's boot step as one command

**Compatibility status of the owner's copy (reported 2026-10-19)**
* `ttt2-iso.exe identify`, built by the owner with Visual Studio 2022, on the owner's disc image: title ID
  `4E4D0859`, media ID `5D762D06`, version `0.0.0.2`, normal retail AES-128-CBC encryption, basic compression,
  and the "Matches" line for the USA/Europe retail disc. The match is decided by the SHA-1 of `default.xex`, so
  TTT2Recomp's published hash `a942090b...` is now confirmed by a real copy (`known_titles.hpp` records the
  confirmation and the media ID; `identify` prints a media-ID check against it). The owner's copy is the image
  TTT2 Web recompiled. **Validated on the real files so far: disc identification only.** Not yet: the Title
  Update 3 steps (the owner does not have the update), the recompilation, the host build, the first boot.
* Consequence: the pipeline gained a base-image mode (no `-TitleUpdate`): steps 3 and 5 are skipped and the
  switch-table finder, codegen, host build, extraction and boot run on the retail executable 0.0.0.2, labelled
  as such in the report; the host logs the base version as a known case instead of a warning. TTT2 Web's
  function boundaries were produced for this image, so the second codegen pass applies to it. The CI dry run
  exercises the base mode on the synthetic fixtures (PowerShell 5.1). Title Update 3 remains the target for
  online play (both players need the same version); the checklist documents the legitimate way to obtain it
  (the owner's console with the disc, Xbox Live's update service, the file copied off the console's storage).
* **The owner's first full run (base-image mode, 2026-10-19)**: identification and the import analysis succeeded
  and all eight helper addresses match TTT2 Web (so its function boundaries apply to the owner's image); then the
  run failed at the recompilation: the pipeline copied the executable as extracted (encryption 1, compression 1)
  to `recomp/game/default.xex`, the finder refused it, no `ttt2_switch_tables.toml` was written, codegen failed on
  the missing include and the host build stopped. Root cause: the pipeline had no step that produces the plain
  form the finder and codegen read (the tools decrypted in memory for hashing and imports only), and the
  synthetic fixture's `default.xex` was plain, so no test covered the retail form. **Validated on the real files
  so far: disc identification, import analysis, helper fingerprint.** Not yet: recompilation, host build, boot.
* Fix (this PR): `make_plain_xex` / `verify_plain_xex` in `src/xex/basefile`, `ttt2-xexinfo --write-plain
  <out>` (headers kept byte for byte, FILE_FORMAT_INFO set to none / none, the decrypted and decompressed image
  appended, the written file read back and verified, a failed output deleted, the input never written), pipeline
  step 5c that prepares `work\default_base_plain.xex` (or `default_tu3_plain.xex`) and hands it to the finder,
  codegen and the host build, `Fail-Pipeline` (a `CRITICAL:` line, the report written, the script stops) for every
  stage the later ones depend on, the finder's refusal naming the step, the synthetic disc executable now in the
  retail form (encrypted + basic-compressed) with ctest regressions, and the SDK workflows running the whole
  chain on a retail-form wrap of the license-free program (`tests/tools/make_retail_xex.cpp`): wrap -> 5c ->
  finder -> codegen -> host -> boot on the plain file and on the retail-form file.

* **The owner's second run (with the step 5c fix, 2026-10-19)**: plain image prepared and verified, 596 switch
  tables (18 bounded or byte-switch dispatches unrecognised), ReXGlue analysed the whole executable (49,708
  functions sealed, 13 not) and stopped on 14 `UnresolvedCall: b T from S - target not in any function` errors;
  host build and boot not reached. **Validated on the real files so far: identification, imports/helpers, the plain
  image, the finder, ReXGlue's analysis up to those errors.**
* Diagnosis (`docs/RESEARCH.md` 8b): a ReXGlue 0.10.0 analysis limitation (identical code at upstream `bd833a2`):
  `b` targets are never registered as functions (only `bl` targets), and gap fill misses the code after a
  function's last branch. Thirteen targets are TTT2 Web function starts reached only from 1-2 instruction thunks
  (missing boundaries); one branch is internal to TTT2 Web's 0x82959A80+0x78, which gap fill cut in two (wrong
  range). Whole-image comparison: 17,656 of TTT2 Web's 18,351 starts coincide with ReXGlue's; all 1,123
  disagreements are ReXGlue gap-fill guesses; 695 TTT2 Web functions (15 KB) stay unregistered, 13 of them reached
  by direct branches.
* Fix (this PR): `recomp/tools/resolve_functions.py` (`codegen` loop, `verify`, `boot`): each error becomes a
  `[functions]` entry only when the branch decodes from the image and TTT2 Web's table (fingerprint-checked against
  ReXGlue's own helper detection) or `.pdata` agrees; entry points get no size (ReXGlue finds the extent), internal
  branches get the reference function's extent; everything else is reported. Output
  `recomp/game/ttt2_resolved_functions.toml` (included by the default manifest, image-SHA-1-stamped, git-ignored).
  On the owner's log: 13 entries plus 0x82959A80+0x78, no conflicts. The pipeline (step 6b), `build-host.ps1` and
  the SDK workflows run codegen through it. Runtime counterpart: the triage recognises the runtime's
  `[FATAL] Call to invalid or unregistered function` trap, `Ttt2App` turns `abort()` into a logged exit with code 4
  (no C runtime dialog on Windows), and pipeline step 9b verifies the called address from the boot log, rebuilds
  and boots again (`-BootRounds`, default 3). Also: the pipeline runs its child scripts (`build.ps1`,
  `build-host.ps1`, `run-host.ps1`) in a child PowerShell, so their native tools' stderr cannot abort them under
  Windows PowerShell 5.1, and strips PowerShell's error colouring and `RemoteException` noise from the report.

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
* **SDK snapshot evaluation and the mechanism for it.** Upstream's `development` branch (`bd833a2`, 24 commits past
  the pinned v0.10.0) was built here, installed as a package and run through the whole license-free suite: its own
  PPC tests (1462/1462), instruction coverage (unchanged, 64 of 66), the codegen smoke test with native runs (all
  cases; the SDK-only jump tables are still wrong, the finder stays mandatory), the host built unchanged against it
  and the normal, stall and crash boot tests. Relevant fixes for TTT2 are listed in `docs/RESEARCH.md` 4.1 (atomic
  `stwcx.`, fences, VMX pack aliasing, XamContent results, leaked kernel objects). The only source change needed
  was in the headless harness (`BuildLogConfig` lost a parameter): it now dispatches on the declared signature and
  builds against both. `REXGLUE_SDK_COMMIT=<sha>` / `-Commit <sha>` on the fetch scripts and the `sdk_commit`
  input of both workflows evaluate any commit without touching the pin; `docs/decisions/0002` records the pin
  policy and the checklist. The pin stays at v0.10.0 for now (no failure points at it yet); the bump is a
  one-PR step when the first boot report asks for one of those fixes.
* **Input requirements recorded** (owner, 2026-10-19): full keyboard play with the owner's Tekken 8 layout (WASD;
  Numpad 4/5/1/2 for 1/2/3/4; Numpad 6 = 1+2; Numpad 3 = 3+4; B = Start; V = Back) and Tag on Numpad 8, saved
  profiles, exact simultaneous presses, opposite-direction handling, keyboard in every screen, low latency, Xbox
  and PlayStation pads alongside, rollback-ready inputs, the game's original input behaviour preserved and no
  Tekken 8 mechanics. `docs/INPUT.md` holds the requirements, the measured state of the SDK's input system (its
  keyboard driver, device merge, driver interface) and the design (own `KeyboardPadDriver` in the SDK's
  `InputSystem`, TOML profiles in Tekken terms, neutral opposite-direction default, press latch, macro keys,
  `PlayerInput` words for the netcode); ADR 0003 records the decision; ROADMAP Milestone 5 lists the steps in
  order. Native game execution keeps priority; no input code was written yet.
* **Input core, Milestone 5 step 1** (`recomp/src/input/input_core.{h,cpp}`, SDK-free): the owner's default
  profile with Tag on Numpad 8, profile files (TOML subset, serialize/parse with line-numbered errors), key
  conflicts, and `KeyboardPad`: presses latched between polls (a tap between two polls is reported once),
  opposite-direction policies (neutral default; last input, first input, up/down priority), macro keys pressing
  both limb buttons, the layout table to pad bits, `PlayerInput` words and press-age statistics. 12 doctest cases
  / 136 assertions, compiled into the host too. Not wired to the SDK yet (step 2), so the game cannot read it.
* Kernel-surface check repeated against a fresh TTT2 Web checkout (`4d6dd61`): its 135 `GUEST_IMPORT` functions
  (a macro parameter had been counted as a 136th name; `recomp/tools/ttt2web_kernel_functions.txt` fixed) are all
  implemented (130) or stubbed (5, overridden) by the pinned runtime, and the seven kernel variable imports its
  loader fills (`KeTimeStampBundle`, `XboxHardwareInfo`, `XboxKrnlVersion`, `XexExecutableModuleHandle`,
  `ExLoadedCommandLine`, `VdGpuClockInMHz`, `VdHSIOCalibrationLock`) are all among the runtime's 24 mapped
  variable exports (`docs/RESEARCH.md` 8a). No kernel preparation is left that does not need the owner's log.
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
* Input core: clang 18 and GCC 13 `-Werror` builds, 64 doctest cases / 749 assertions in all (12 / 136 new); the
  host compiled the same source against the installed SDK package.
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
* PR #7 runs: CI 10/10 green on every head after the PowerShell 5.1 capture fix (`873294c`); SDK validation run
  39231479253 (`a4d9cef`) green with the boot step under `pwsh` + Xvfb on both images ("clean exit"; "stall ...;
  the window was closed afterwards" after the 8 s close request, exit 0 in 8.2 s); Windows run 39231482868
  (`a4d9cef`) green: **first run of `run-host.ps1` on Windows**, both forms passed, the `taskkill` close through
  `Start-Process` ended the host with exit 0 in 8.3 s; Windows run 39232181325 (`42bd2b3`) green: the package
  relocated, the licence bundle added (23 components with licence files at that commit; 54 MB unpacked, 13.5 MB
  as the `rexglue-sdk-install-win-amd64` artifact), `ttt2.exe` built from the moved copy, every boot form passing.
  The Windows workflow on `main` after the PR #6 merge (run 39230244565, `5acfb19`) was green too. Windows run
  39234438770 (`15e09bd`) green with the completed licence bundle: 25 entries (the SDK's own, 23 components'
  files incl. `crypto/des` and `libmspack` one level down, and `NOTICES.txt` naming the nine header-only
  components whose licence text lives in their sources: disasm, disruptorplus, dxbc, dxc, ffmpeg-overlay, pe,
  renderdoc, stb, tiny-aes-c).

* Windows run 39237113902 on `main` after the PR #7 merge (`8d21247`): green.
* SDK snapshot `bd833a2`, Linux container: `ppc_tests` "All tests passed (5757 assertions in 1462 test cases)";
  `check_instruction_coverage.py --sdk <snapshot>`: 64 implemented, 2 missing (same as the pin); codegen smoke test
  `--exec`: all five cases, native runs 12/12 with the finder's tables, overrides reached (0, 1317), no stubs; host
  built from the snapshot's package: boot tests normal/stall/crash PASS. The same smoke run against the pin with
  the new harness shim: identical results. Fetch-script override: accepted on a snapshot checkout, refused without
  it, pinned checkout unaffected (sh and pwsh 7.4).

* PR #8 dispatched runs on `d9d21a1`, all green: SDK validation with `sdk_commit=bd833a2` (run 39240052895:
  `rexglue 0.10.0.24-dev.gbd833a2`, PPC tests, smoke test with native runs, host built against the snapshot's
  package, boot tests, boot step "clean exit" / "stall ...; closed afterwards") and with the pin (run
  39240056253); the Windows workflow with the snapshot plus `native_run` and `build_host` (run 39240060409:
  tool built, smoke test, snapshot package 54 MB with the licence bundle, `ttt2.exe` built and booted from it,
  every boot form) and with the pin (run 39240064074). CI 10/10 on the head.

* Base-image preparation, Linux container: 69 doctest cases / 834 assertions (5 cases new: plain form of the
  encrypted fixture, idempotence and the unencrypted variant, refusals, tamper detection, the compression helper);
  24 ctests (10 new) on the retail-form synthetic executable: `--write-plain` verified with the image SHA-1
  `f1a74890...` of `make_import_test_image()`, the plain output parsed as none / none with the same imports and
  helpers, the same-path refusal, the delta-patch refusal leaving no file, the finder refusing the retail form
  (exit 2, message naming the step) and accepting the plain output, the wrapper round trip; GCC 13 and clang 18
  `-Werror`. End to end with the installed pinned SDK: `imports_ttt2` packed -> wrapped into the retail form (4
  blocks, 262144-byte image) -> finder refuses it -> `--write-plain` verified -> finder accepts (results identical
  to the packed original) -> `rexglue codegen` exit 0 -> host built -> `host-boot-test: PASS` on the plain root
  **and on the retail-form root** (the runtime decrypts and decompresses the executable itself). Pipeline script:
  PowerShell 7.4 parse clean; the stop-on-critical path exercised (missing tools -> `CRITICAL:` line, report
  written, throw).

* Function-boundary resolution, Linux container: `recomp/tests/ppc/tailcall_ttt2.s` (three shapes) with the
  pinned SDK: pass 1 fails with exactly the two expected `UnresolvedCall` errors; the resolver adds one entry point
  and one function boundary and codegen succeeds (7 functions); the headless harness traps on the pointer-only leaf
  (exit 134, runtime FATAL line), `resolve_functions.py boot` adds it, codegen and the harness then give
  1002 112 24 406 35. The real host on the same program: exit code 4 with the FATAL and abort lines, `run-host.ps1`
  (pwsh 7.4) triage "fatal: the game called 0x82010108 through a pointer", resolution from the host's own log,
  rebuild, clean exit 0. The resolver's decisions on the owner's actual log (image decode excepted): 13 entries and
  0x82959A80+0x78, fingerprint 8 of 8. Python: 38 tests (17 new). The pipeline's boot loop exercised under pwsh 7.4
  with stubbed steps and the real resolver (add/rebuild/reboot, no progress, round limit, no -Codegen).

**Not verified / open**
* Codegen through the resolver, the host build and the boot on the owner's real executable (the owner reruns).
* The 18 dispatches the finder could not read are not codegen errors; if ReXGlue compiled them as indirect calls, a
  boot reaching one stops on the runtime trap with an address inside a function, which step 9b reports instead of
  resolving: the finder then needs those shapes.
* The owner's first boot is the only test of the snapshot's kernel and GPU changes with the real image; a bump
  stays a one-PR step (`docs/decisions/0002`).
* A package built on the runner's LLVM consumed by an owner's different LLVM 20+ minor version is expected to
  link (same MSVC ABI) but is unverified until the owner's run.
* Everything on license-free programs; the real image still waits for the owner's run.

**Blocked / pending**
* Owner's files for Milestone 3 steps 3-5 and the first boot (Milestone 4): the owner path is now the
  `rexglue-sdk-install-win-amd64` download plus
  `scripts\owner-pipeline.ps1 -Iso ... -Codegen -BuildHost -SdkInstall <dir> -ExtractGame -Boot` (base-image
  mode; `-TitleUpdate ...` once the update is available) and the three files it names (`work\boot-triage.md`,
  the log copy, `work\boot-report.txt`) plus `work\report.txt`. The owner's rerun after the step 5c fix is the
  next real-data checkpoint.

**Next task (resumable state)**
* PR #8 (SDK snapshot evaluation mechanism, harness shim, ADR 0002): CI plus the dispatched pin and snapshot runs
  green, merge, restart the branch. Then iterate on the owner's first-boot report as described in session 4's next
  task; if that report points at atomics, fences, VMX pack or XamContent behaviour, the snapshot bump
  (`docs/decisions/0002`) is the first candidate fix.

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
