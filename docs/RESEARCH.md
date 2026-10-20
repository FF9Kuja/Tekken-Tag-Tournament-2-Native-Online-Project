# Milestone 1 - Technical Research

*Status: complete for the first session (2026-10-18). Facts below were gathered by cloning and reading the
repositories named, by building the tools where possible, and by reading the published wiki and release pages.
Every claim is labelled as **verified** (reproduced here) or **reported** (stated by another project, not yet
reproduced).*

## 1. Summary of findings

1. **A native TTT2 port already exists and plays.** `shihabal3amri/tekken-tag-tournament-2-pc` ("TTT2Recomp",
   first release 2026-10-13, v0.1.6 on 2026-10-18) is a complete, released native Windows port built with
   XenonRecomp, its own hand-written runtime (D3D11, XAudio2, FFmpeg XMA) and an installer that builds the game
   from the player's own disc image plus Title Update 3. Online play uses the game's *original delay-based*
   netcode over Steam networking. It has **no rollback** and **no license file** (all rights reserved by default),
   so it is a proof of feasibility and a knowledge reference, not code we can reuse.
2. **The primary reference, Tekken 6 Native Online (Ekspym), is a ReXGlue SDK project.** Its published source
   (also **no license file**) shows a working design for GGPO-style rollback on top of a recompiled Xbox 360 game:
   mid-asm hooks at the frame boundary and the input entry builder, snapshots of the game's memory ranges with
   dirty-page tracking inserted into the generated C++ by post-codegen patch tools, a record/replay "sound
   boundary" so the audio library is never rolled back, controlled RNG seeds, and a lobby/launcher. Its build model
   (player builds the game locally with a bundled compiler; the package ships no game content) is the model this
   project adopts.
3. **A CC0 (public-domain) TTT2 codebase exists.** `monty488/tekkenwebtournament2` ("TTT2 Web") is an in-progress
   XenonRecomp-based port to WebAssembly with a 14.6k-line hand-written runtime, a scan tool, a jump-table
   finder tailored to TTT2's compiler output, and ~70 added PowerPC instruction implementations. Its CC0 license
   makes it the only TTT2-specific code we may legally borrow from, and its documentation records concrete
   TTT2 runtime behaviours (thread set, vblank/DPC needs, XAM calls) that any runtime must satisfy.
4. **Toolchains verified on Linux in this session:** XenonRecomp + XenonAnalyse (MIT) built with Clang 18 here;
   the ReXGlue SDK v0.10.0 (BSD-3) codegen tool built with Clang 20 here, and its `init` and PPC instruction-test
   generator (`recompile-tests`) ran successfully (`docs/STATUS.md`). This container's Clang can also emit
   big-endian PowerPC ELF objects, which the SDK loads but cannot analyse without a PE `.pdata` directory.
5. **Recommended architecture: ReXGlue SDK as codegen + runtime, with the XenonRecomp family as an independent
   cross-check and as the title-update patch tool.** Rationale and risks in section 7 and
   `docs/decisions/0001-use-rexglue-sdk.md`. The decisive factor: ReXGlue ships a Xenia-derived runtime (kernel,
   memory, VFS, GPU, audio, input); XenonRecomp ships none, and the two existing TTT2 runtimes are either
   unlicensed (TTT2Recomp) or targeted at WebAssembly with a software rasterizer (TTT2 Web).

## 2. Environment used for this research

| Item | Value |
|---|---|
| Container | Ubuntu 24.04.5, x86-64, 4 cores, 15 GiB RAM |
| Compilers | GCC 13.3.0, Clang 18.1.3 (targets include `ppc32`/`ppc64`), LLD 18 |
| Build tools | CMake 3.28.3, Ninja 1.11.1, Python 3.13 |
| Missing | Any Windows toolchain (MSVC, Windows SDK, MinGW), Wine, a GPU, game files |
| Network | GitHub and Ubuntu package archives reachable |

Consequences: everything that targets Windows (the game host, the launcher, the player-side builder) can only be
**configured and cross-checked** here; compiling and running them requires a Windows PC or the Windows GitHub
Actions runners configured in `.github/workflows/ci.yml`.

## 3. Reference projects

### 3.1 Tekken 6 Native Online (Ekspym)

Repositories inspected: `Ekspym/Tekken6-native-online` (README + releases; v2.1.6 to v2.3.0 released
2026-10-15..18) and `Ekspym/tekken6-native-online-source` (source published "without the full development
history"; **no LICENSE file**; credits `mahmoud-mess/tekken6-recomp`, "used with the author's permission").

**How the port works (verified from source):**

* **Recompilation.** The game's `Default.xex` (Tekken 6 USA/Europe Rev 1, title `4E4D07FC`, SHA-256
  `ee138a4f...` checked by the builder) is translated to C++ by `rexglue codegen` (ReXGlue SDK pinned at
  v0.10.0 / commit `f5337cd`), driven by `tekken6_config.toml`: about 40 explicit function boundaries, 12 switch
  tables (several "proved by an independent XenonRecomp pass"), one `indirect_calls` entry, `[rexcrt]`
  `memcpy`/`memset` redirected to host implementations, and nine `[[midasm_hook]]` entries.
* **Runtime.** The ReXGlue runtime (Xenia-derived kernel/XAM, memory, VFS, D3D12 "Xenos" GPU plugin, XMA audio via
  FFmpeg, SDL3/XInput input, ImGui overlay) with a 97-file patch to the SDK (`patches/rexglue/0001-...patch`,
  10.5k lines) for codegen options, deterministic audio stepping, lock-free waits, snapshot exports, a presenter
  overlay, the llvm-mingw build and a fused resolve shader. The app is a `rex::ReXApp` subclass
  (`Tekken6App`) that overrides `OnPreSetup`, `OnPostLoadXexImage`, `OnPostLaunchModule`, `OnCreateDialogs`.
* **Graphics / audio / input / system calls.** All provided by the SDK; the project adds internal-resolution
  hooks (`ApplyTekkenInternalResolution` at the two guest stores that initialise 1024x576), practice-mode HUD,
  name plates, and an "audio step" that advances the sound engine in lockstep with simulated frames.
* **Rollback.** A GGPO fork (MIT; prediction 3 frames; inputs are `XINPUT_GAMEPAD` structs). The game runs its
  *offline Versus mode*; `XamInputGetState` for users 0/1 is fed from the GGPO session. Hooks:
  `T6FrameHook` at the head of the main loop (frame boundary: advance the previous frame, `ggpo_idle`, add local
  input, synchronize, run the frame; a rollback = restore snapshot + call the per-frame task-tree walk again),
  `T6InputHook` in the pad entry builder after both players' input entries are built, `T6SeedHook` /
  `T6CrtSeedHook` to replace `srand` seeds with an agreed seed. Snapshots copy the XEX data/bss
  (`0x82726000-0x82DB0000`) and the game heaps (`0xA7290000-0xABA90000`) guided by **dirty-page maps written by
  every guest store** (post-codegen patch tools `pagemap_patch.py`, `rawstore_patch.py`, `faststore_patch.py`
  rewrite the generated C++), after waiting for the GPU ring/fences/swap to be idle and the job workers to be idle.
  The sound library (CnuSound2 + statically linked XAudio2 ranges) is **not** rolled back: `sndcall_patch.py`
  routes every game->sound-library call through `t6_snd_call`, which records calls on live frames and replays
  them on re-simulated frames. Cosmetic `rand()` callers (crowd, particles) get their own RNG state. Desync
  diagnostics: per-range FNV-1a hashes per step, `T6_SNAPTEST` (save, run k frames, restore, re-run, compare),
  `T6_REC`/`T6_PLAY` input recording. Sessions start at the first frame boundary of the fight loading, with the
  file-request queue forced idle before every frame so loading is deterministic on both sides.
* **Multiplayer architecture.** Direct UDP peer-to-peer (GGPO transport); the launcher opens the port by UPnP,
  detects CGNAT/double-NAT, and talks to a lobby server over TLS with a pinned self-signed certificate (server not
  published). Players are matched only with the same package version; the player's exe is built reproducibly
  (`-ffile-prefix-map`, `-g0`, no PE timestamp) so the launcher can pair players by exe hash. A "park" command keeps
  the game process alive between matches; the launcher sends start lines with characters/stage/names.
* **Launcher / installation.** Qt 6 launcher (MSVC). "BUILD GAME" runs `builder/build_native.py`: checks the ISO
  (`xdvdfs.exe` extracts `Default.xex` and verifies its SHA-256), extracts the disc to `native\game`, runs
  `rexglue.exe codegen`, applies the four patch tools, compiles the generated `recomp.N.cpp` files with a bundled
  llvm-mingw `clang++` (incremental, keyed by content hashes; `-march=x86-64-v2 -O3 -std=c++23`, PCH), links against
  prebuilt libraries (`libt6prebuilt.a`, GGPO, SDL3, spdlog, fmt, xxHash, `librexruntime.dll.a`) and copies the
  runtime DLLs. `--update` recompiles only changed files; `--relink` relinks in seconds. Auto-update is in the
  launcher. The package contains no game content.

**Reusable vs. adaptable vs. must-be-rewritten (for TTT2):**

| Component | Reusable? | Notes |
|---|---|---|
| ReXGlue SDK | Yes (BSD-3) | Same dependency; our own SDK modifications must be re-derived (the T6 patch is unlicensed). |
| GGPO | Yes (MIT) | Use upstream GGPO; re-apply equivalent changes ourselves. |
| Builder pipeline design | Design only | Re-implement (Python, our code). |
| Netplay sources (`t6_*.cpp`) | **No** (no license) | Study the design; TTT2 needs different addresses and ranges anyway. |
| Codegen config | **No** (game-specific) | TTT2 needs its own function/switch-table/hook config. |
| Launcher | **No** (unlicensed, Qt) | Own implementation; Qt is an option (LGPL) but adds a large dependency. |

### 3.2 mahmoud-mess/tekken6-recomp

"Original open-source static recompilation of Tekken 6" (17 stars, 17 commits, last 2026-10-16). **No license
file.** It documents a **Linux-hosted Windows build** with llvm-mingw and a `cmake/windows-clang.cmake` toolchain,
a portable "Tekken 6 Builder.exe" that bundles LLVM-MinGW, CMake, Ninja and embeddable Python, D3D12 ROV and
Vulkan FSI rendering paths, FMA3 codegen and an internal-resolution chooser. Its Linux build commands for the SDK
codegen tool were followed here (section 4).

### 3.3 TTT2Recomp (shihabal3amri/tekken-tag-tournament-2-pc)

46 stars, 10 commits, releases v0.1.1 (2026-10-13) .. v0.1.6 (2026-10-18). **No license file** (reference only).
Verified from source and README:

* **Toolchain:** XenonRecomp at commit `ddd128b` plus a 3.4k-line patch (XenonAnalyse function/jump-table
  detection, recompiler, `ppc_context.h`, the PowerPC disassembler); XenonAnalyse on the TU3-patched image;
  `gen_missing_switches.py` restores switches XenonAnalyse misses ("535 TTT2 swapped-order jump tables");
  `gen_missing_functions.py` harvests `// ERROR <addr>` branch targets and declares them as functions until none
  remain (XenonRecomp issue #34 style mis-split boundaries); `gen_kernel_stubs.py` stubs every kernel/XAM import;
  recompiled code compiled with clang-cl at `-O0..-O2`, `-march=x86-64-v2`, `-ffp-contract=off`.
* **Runtime (18k lines, `port/src/runtime`):** kernel/XAM/XLive/XSession/XSocket HLE, guest heap
  (virtual `0x00100000-0x3F000000`, physical `0xA0000000-0xC0000000`), 4 GiB + 16 MiB guest reservation with a
  write-protected null page, Xenos PM4 ring reader, **D3D11 hardware renderer plus a CPU software rasterizer that
  interprets Xenos shader microcode**, XenosRecomp for shader translation, XMA via a minimal FFmpeg n7.1, XAudio2
  output, keyboard/XInput, save data in `%LOCALAPPDATA%\TTT2Recomp`.
* **Online:** the game's own Xbox LIVE session flow (XSession/matchmaking) emulated over a packet transport:
  SteamNetworkingMessages via a helper process (`--steam-bridge`, Spacewar app ID) or loopback UDP for two local
  instances. The game's original delay-based netcode runs unchanged; **no rollback**.
* **Game facts:** title ID `4E4D0859`; required Title Update 3 is `TU_174Q22P_0000008000000.00000000000O2`
  (targets version 0.0.3.2; TU1 `...82` -> 0.0.1.2, TU2 `...G2` -> 0.0.2.2); retail base `Default.xex` SHA-1
  `a942090b7c293d0ce63346ed8a2974448135d0dd` (**confirmed 2026-10-19** by the owner's disc through `ttt2-iso
  identify`: media ID `5D762D06`, version 0.0.0.2, normal retail AES-128-CBC, basic compression); TU3-patched
  decrypted image SHA-1 `66adbc1a7744ec840a3ae8e51f8aeb677140d789` (still **reported** only: the owner has no
  Title Update 3 yet); XGD3 redump ISO game partition at `0x2080000` (XGD2 `0xFD90000`); 45 top-level disc files
  of which 26 are LZX containers (magic `0x0FF512ED/EE`) expanded at install to sizes from `nrfo.bin`;
  TU3's `data100.bin` + `data100.ofs` become the `update:` device; ~13 GB installed.
* **Gameplay facts relevant to rollback:** the game adds **2 frames of input delay in every offline mode** through
  a per-player FIFO (`sub_8225BFC0`; delay word at `obj+0`); Practice's "Input Delay" setting uses 5/8/11/14; online
  uses its own value. Removing it changes reaction latency from ~94 ms to ~52 ms (reported measurements).
* **Register save/restore helper addresses on the TU3 image (reported):** `restgprlr_14 0x82BD37D0`,
  `savegprlr_14 0x82BD3780`, `restfpr_14 0x82BD446C`, `savefpr_14 0x82BD4420`, `restvmx_14 0x82BD5688`,
  `savevmx_14 0x82BD53F0`, `restvmx_64 0x82BD571C`, `savevmx_64 0x82BD5484`.

### 3.4 TTT2 Web (monty488/tekkenwebtournament2)

**License: CC0-1.0** (public-domain dedication). 48 commits, last 2026-10-18. XenonRecomp + XenosRecomp +
extract-xiso + Emscripten/WebGPU. Status (README): "playable natively, booting in the browser"; menus, character
select and fights at 60 fps natively; audio, saves and sign-in missing. Verified from source and docs:

* `tools/ttt2-scan` (C++, XenonUtils): decrypts/decompresses `default.xex`, finds the eight save/restore helpers by
  byte pattern, lists imports and variable imports, writes `image_info.toml`.
* **Base-image (no TU) helper addresses:** `restgprlr_14 0x82BAF6A0`, `savegprlr_14 0x82BAF650`,
  `restfpr_14 0x82BB033C`, `savefpr_14 0x82BB02F0`, `restvmx_14 0x82BB1558`, `savevmx_14 0x82BB12C0`,
  `restvmx_64 0x82BB15EC`, `savevmx_64 0x82BB1354` - different from the TU3 image, as expected.
* **Jump tables:** "XenonAnalyse's fixed patterns find none in TTT2, because its compiler schedules the dispatch
  as `lis, rlwinm, addi, lwzx` rather than `lis, addi, rlwinm, lwzx`"; their `tools/find_switch_tables.py`
  symbolically executes the instructions before each `bctr` and finds **526** tables and fixes 69 leaf-function
  boundaries. (TTT2Recomp reports 535 with its own patched analyser.)
* **Instructions:** "TTT2 uses about 70 instructions Sonic Unleashed didn't, mostly halfword VMX, update-form
  loads/stores and half-float pack/unpack" (`patches/XenonRecomp-0002-ttt2-instructions.patch`, unverified by
  them against Xenia's tests). Four trap sites remain in data-as-code (`macchwu`, `maclhwu`, `blrl`).
* **Runtime behaviours TTT2 needs** (from their roadmap): ~20 game threads (`nuscMain`, `nuscFileAsyncRequest`,
  ...), static TLS from the XEX TLS directory, dispatcher objects with NT wait semantics (FIFO hand-off), a 60 Hz
  vblank graphics interrupt with DPCs on their own thread, a PM4 ring/command processor, XAM system UI /
  enumerator / content calls completing `XOVERLAPPED` requests, timebase 49.875 MHz, and fixes for "audio callback
  argument, wakeup storms, critical sections" that otherwise stop frame submission.

## 4. Toolchain evaluation

### 4.1 ReXGlue SDK (`rexglue/rexglue-sdk`)

* **License:** BSD 3-Clause (Tom Clay, 2026; portions from Xenia, BSD 3-Clause). **Verified.**
* **Status:** v0.10.0 tagged 2026-08-21; nightlies through 2026-10-02 (`0.10.0.24-dev`); README warns "early
  development ... significant changes and breaking public API updates". Linux (amd64/arm64), Windows and macOS CI.
* **`development` branch evaluated against the pin (2026-10-19; `bd833a2` = `0.10.0.24-dev`, 24 commits past
  v0.10.0, `main` still at the release).** Commits relevant to TTT2: `stwcx.` migrated to a `std::atomic` CAS
  with CR0.so dropped (`7f7c92e`), `sync`/`lwsync`/`eieio` emitted as fences (`bd833a2`), register types made
  unsigned (`ea222e9`), `vpkuwus`/`vpkuhus` packed through a temporary when the destination aliases a source
  (`6319e23`; TTT2 uses the halfword VMX pack family), XamContent results compared as `X_ERROR_SUCCESS` instead
  of NTSTATUS (`ee20900`), leaked event/socket/completion-port objects released (`0c7b01a`), file overwrites
  truncated in place (`01ac2a3`), `writable_code_segments` (`5e2dae1`), plus input (keyboard passthrough,
  vibration, hotplug), UI (fullscreen switching) and logging (`LogConfig` path/budget/flush) features, SDL3
  3.4.14. No jump-table detector change and no new instruction builders: `check_instruction_coverage.py` reports
  the same 64 of 66 (`macchwu`/`maclhwu` missing) and the SDK-only recompilation of `switch_ttt2.s` still runs
  wrong (exit 132) while the finder's tables give 12 of 12, so the finder stays mandatory. Public API: only
  `BuildLogConfig` lost its `log_file` parameter (the harness dispatches on the signature); `ReXApp` gained
  `OnConfigureLogging(LogConfig&)`; every hook, cvar and cmake entry point the host uses is unchanged, and
  `recomp/` built against the snapshot's installed package without a source change. Measured on the snapshot
  here: its own `ppc_tests` 1462 cases / 5757 assertions pass (v0.10.0: 1458 / 5725), the codegen smoke test
  with native runs passes all cases, the host passes the normal, stall and crash boot tests. Also seen in its
  headers: `IsHostPageWriteWatched` on the memory interface (write-watch infrastructure, of interest for the
  rollback snapshots of Milestone 6). The pin was not moved: `docs/decisions/0002` sets the rules, and the
  `sdk_commit` workflow input re-runs this evaluation on any commit.
* **What it is:** a static recompiler (`rexglue codegen`) **and** a runtime derived from Xenia with the JIT
  replaced by a function dispatch table: kernel (`xboxkrnl` export table 936 lines, `xam` 1749 lines), memory, VFS
  (`game:`, `d:`, `update:` mounts), threading, XMA audio decoder (FFmpeg), SDL3 audio, SDL3/XInput/mouse-keyboard
  input, D3D12 and Vulkan Xenos GPU backends (Xenia's command processor, shader translator, render-target and
  texture caches), ImGui-based overlay/dialog UI, `ReXApp` application framework with lifecycle hooks
  (`OnPreSetup`, `OnLoadXexImage`, `OnPostLoadXexImage`, `OnPreLaunchModule`, `OnPostLaunchModule`,
  `OnGuestThreadExit`, detached-renderer mode).
* **Codegen pipeline (wiki + source):** seven analysis stages (Register, Scan, Discover, GapFill, Discover, Merge,
  Validate) over PDATA entries, a signature scanner that auto-detects the MSVC save/restore helper families (no
  manual addresses, unlike XenonRecomp), block-based discovery, an RTTI vtable scanner, jump-table detection
  (absolute / byte-offset / half-word / computed; manual `[[switch_tables]]` fallback), validation of every branch
  target (`--force` emits error comments instead of failing), C++23 output split into `recomp.N.cpp` files with a
  PCH, `init`/`register` units, and a CMake fragment. Config: `[functions]` (size/end/parent chunks/name),
  `[[switch_tables]]`, `[[invalid_instructions]]`, `[[midasm_hook]]` (registers, return/jump variants,
  `after_instruction`), `[rexcrt]` (CRT functions redirected to host), `indirect_calls`, `*_as_local` register
  optimizations, `enable_exception_handlers` (SEH wrappers).
* **Title updates:** the wiki lists `patch_file_path`/`patched_file_path`, but **verified by grep of v0.10.0 and the
  2026-10-02 nightly: the codegen has no XEXP handling** (`RecompilerConfig` has no such field). The runtime's
  `XexModule::ApplyPatch` (Xenia heritage) exists for run-time patching. For TTT2, which must be recompiled from
  the TU3-patched image, a **pre-patch step** is required (section 4.2).
* **Build requirements:** CMake 3.25+, Ninja, Clang 18+ (CMake check) / "Clang 20+" (wiki), C++23, `-mcmodel=large`
  on Linux x86-64 for 35 MB+ recompiled executables, 22 git submodules (FFmpeg, SDL3, Vulkan stack, glslang,
  SPIRV-Tools, ImGui, spdlog, fmt, tomlplusplus, inja, xxHash, simde, ...; 709 MB shallow). On Ubuntu it also needs
  GTK3 and the SDL3 X11/XCB/Wayland/audio development packages (verified here by trial: the configure step failed on
  missing `libgtk-3-dev`, `libxss-dev` and `libx11-xcb-dev` before they were installed).
* **Verified here (2026-10-19):** the `rexglue` tool builds on Ubuntu 24.04 with Clang 20 (not 18) and
  `-mssse3`; `rexglue init` and `rexglue recompile-tests` work; `rexglue codegen` loads a plain PowerPC ELF but
  requires a PE `.pdata` exception directory, so license-free codegen inputs must be wrapped as XEX2/PE images.
* **Known project-specific needs:** Tekken 6 NO carries a 97-file patch against the SDK. Expect TTT2 to need its own
  SDK modifications (deterministic audio stepping, snapshot support, exports used by netplay), to be maintained as
  patches or an SDK fork under the BSD-3 license.
* **Community note:** a Reddit discussion of the GoldenEye 007 ReXGlue port called the SDK a "vibe coded wrapper
  for Xenia"; the author disputes it. Treated here as a maturity risk, not a blocker: the SDK is 1,300+ files of
  Xenia-derived code with CI on three platforms and at least two shipped fighting-game ports.

### 4.2 XenonRecomp / XenonAnalyse / XenonUtils / XenosRecomp (`hedge-dev`)

* **License:** MIT. **Verified.** Last commit 2025-08-04 (`ddd128b`); XenosRecomp last push 2026-03-12.
* **What it is:** a recompiler (`XenonRecomp config.toml ppc_context.h`), a static analyser (`XenonAnalyse`:
  function boundaries from `.pdata` + scanning, jump tables by fixed patterns), and `XenonUtils` (XEX loader with
  retail-key AES decryption, basic/LZX decompression via libmspack, **`XexPatcher` for XEXP delta patches**, xdbf).
  **No runtime.** Patch mechanisms: `PPC_FUNC` weak aliasing to override functions and mid-asm hooks.
* **Limitations (README + TTT2 experience):** manual save/restore helper addresses; jump-table detection tuned for
  Sonic Unleashed (finds none in TTT2 without changes); function boundary mis-splits produce `// ERROR` sites;
  ~70 instructions used by TTT2 were missing (TTT2 Web added them); no MMIO; no exceptions.
* **Verified here:** cloned, submodules fetched, built on Ubuntu 24.04 with Clang 18 (`XenonRecomp`, `XenonAnalyse`
  link; 28 build steps). The MIT `XexPatcher` is the proven route (used by TTT2Recomp's installer) to produce the
  TU3-patched image that both ReXGlue codegen and our validation hashes need.

### 4.3 Other tools considered

| Tool | License | Role here |
|---|---|---|
| Xenia / Xenia Canary | BSD-3 | Emulator; the reference for every kernel/GPU/audio behaviour; not a port approach. |
| LLVM/Clang + LLD | Apache-2.0 with LLVM exception | Required compiler for both recompilers and for generated code. |
| llvm-mingw | Apache-2.0 w/ LLVM exception (+ mingw-w64 licenses) | Redistributable compiler for the player-side build (T6 NO and tekken6-recomp use it). |
| MSVC + Windows SDK | Proprietary (free Community/Build Tools) | Developer builds and the Windows CI runners. |
| CMake / Ninja | BSD-3 / Apache-2.0 | Build system (already in use). |
| Ghidra (+ XEX loader plugins) | Apache-2.0 | Manual reverse engineering of hook points (frame loop, input builder, RNG). |
| IDA Pro | Proprietary | Optional; not required. |
| rexdex/recompiler, N64Recomp | MIT | Historical/other-platform references only. |
| GGPO | MIT | Rollback library (used by T6 NO). Alternatives: own implementation modelled on it. |

## 5. TTT2 facts that shape the design

* Title ID `4E4D0859`, "Tekken Tag Tournament 2 (USA, Europe)" retail base version 0.0.0.2; Title Update 3 ->
  0.0.3.2 is the final balance/content update and the version both existing ports target. **The port must be
  built from the TU3-patched image**, so the pipeline needs the player's TU3 file (STFS container holding
  `Default.xexp` and `data100.bin/.ofs`) in addition to the disc image.
* TTT2's engine is the Tekken 6 engine's successor (Namco "NU" libraries: `nusc*` threads, the same
  `tcb_*` task-tree style seen in T6 NO's hooks). The T6 hook *strategy* (frame-boundary at the main loop's
  task-tree step, input entry builder, `srand` hooks) should transfer; the *addresses* will not.
* Codegen hazards specific to TTT2: non-standard jump-table scheduling (526-535 tables), ~70 extra PPC
  instructions, four data-as-code trap sites, leaf-function boundaries without `.pdata`.
* Runtime hazards: ~20 guest threads with NT wait semantics, 60 Hz vblank/DPC timing the game depends on, XAM UI
  and content enumeration, the original 2-frame input FIFO in offline modes, Xbox LIVE session APIs behind the
  online menus (rollback will bypass them, as T6 NO does, by running offline Versus).
* Size: ~13 GB installed game data; 7 GB+ ISO; a recompiled Tekken 6 took 3-10 minutes to compile on a player's PC.

## 6. Major technical challenges (ranked)

1. **Getting the TU3-patched TTT2 image through ReXGlue codegen cleanly** (jump tables, instruction coverage,
   function boundaries, validation errors) - the first thing Milestone 3 must measure. Fallback: XenonRecomp +
   TTT2 Web's switch-table finder, which is known to work on this exact game.
2. **Booting natively with the ReXGlue runtime** (kernel/XAM behaviours TTT2 relies on; both existing ports needed
   many fixes here; T6 NO needed a 97-file SDK patch).
3. **Deterministic simulation and state restoration**: identifying the exact memory ranges, the audio boundary, the
   worker threads and the GPU idle conditions for TTT2, as T6 NO did for T6. Tag mechanics add state (two
   characters per side, tag assault/crash) but no new architectural element.
4. **Performance**: rollback re-simulates up to N frames per frame; snapshots of tens of MB per frame need the
   dirty-page machinery; TTT2Recomp reached 60 fps only after several optimisation releases.
5. **Graphics fidelity** through the Xenia-derived D3D12 backend (TTT2 uses more effects than T6: fire, tag
   intros, 59 characters' shaders); shader cache stutter on first fights is expected.
6. **Online services** without paid infrastructure: P2P UDP with UPnP + hole punching, a small lobby service later.
7. **Player-side build pipeline on Windows**: bundling a compiler, keeping builds reproducible so peers can be
   matched by exe hash, and keeping the install under ~15 GB.

## 7. Recommended initial architecture

**ReXGlue SDK (BSD-3) for codegen and runtime; GGPO (MIT) for rollback; XenonRecomp family (MIT) for XEXP
patching and as an independent analysis cross-check; TTT2 Web (CC0) as a TTT2-specific knowledge/code source;
own code for ingestion tooling, netplay integration, online services and the launcher.** See
`docs/ARCHITECTURE.md` and `docs/decisions/0001-use-rexglue-sdk.md`.

Why not XenonRecomp + own runtime (the TTT2Recomp route)? It is proven on this game, but the runtime is the
largest piece of work (18k lines in TTT2Recomp, unlicensed; TTT2 Web's 14.6k lines target WebGPU/software
rendering) and ReXGlue already provides a Xenia-class one. Why not fork TTT2Recomp? No license. Why not Xenia
itself? Rollback needs native, instrumentable code and deterministic re-simulation; an emulator's JIT and
threading model work against that, which is exactly why the T6 project recompiled.

## 8. Assumptions, open questions and risks

**Assumptions**
* A1. ReXGlue's codegen can be made to handle the TTT2 TU3 image with configuration (switch tables, functions) and,
  if needed, small SDK patches for missing instructions. *Untested.*
* A2. The ReXGlue runtime boots TTT2 to the menus with a manageable number of kernel/XAM fixes. *Untested; T6 and
  the GoldenEye port suggest it is feasible for games of this generation.*
* A3. TTT2's simulation is deterministic given identical inputs, seeds and audio/loader boundaries (true for T6).
* A4. Players own the disc and TU3 and can provide an ISO and the TU file; the project never ships game content.

**Open questions (to be answered by Milestone 3/4)**
* Q1. Does ReXGlue detect TTT2's jump-table pattern? **Partly, and not reliably (section 8a):** our finder
  (`recomp/tools/find_switch_tables.py`) emits the `[[switch_tables]]` entries instead.
* Q2. Which of the ~70 "missing" instructions does ReXGlue already implement? **64 of 66 (section 8a).**
* Q3. Exact frame-boundary, input and seed hook addresses in the TU3 image.
* Q4. Does the SDK's XMA/XAudio path allow a deterministic "audio step" without the T6 patch approach?
* Q5. Licensing choice for this repository (owner decision; MIT recommended for compatibility with BSD-3/MIT deps).

**Risks**
* R1. SDK churn ("breaking public API updates"): pin an SDK commit; carry patches; upgrade deliberately.
* R2. Legal: never commit game bytes, generated code or TU contents; CI enforces (`scripts/check_repo_hygiene.py`).
* R3. Windows-only validation gap: codegen can be exercised on Linux, but boot/gameplay need a Windows PC with a
  D3D12 GPU and the owner's game files.
* R4. Effort: T6 NO's netplay layer alone is ~30 source files; expect months, not weeks, for Milestones 4-6.

## 8a. Measurements on the pinned ReXGlue SDK (session 3, 2026-10-19, license-free inputs)

All of this was reproduced here with SDK 0.10.0 (`c94f5eb`) built with Clang 20 on Ubuntu 24.04; the test
programs are in `recomp/tests/ppc/` and the checks in `scripts/codegen-smoke-test.sh`.

**Jump tables (answers Q1).** `switch_ttt2.s` holds nine switch dispatches: TTT2's `lis, rlwinm, addi, lwzx`
order, a `bgtlr` bounds check, a compare on an `mr` copy, a recomputed biased index with in-place scaling,
byte-offset and halfword-offset tables, interleaved unrelated instructions, an `ori` low half in a leaf without
`.pdata`, and the `lwzx` operands in both orders. The SDK's detector (`detectJumpTable` in
`src/codegen/function_scanner.cpp`) recognises 7 of 9, but:
* it finds **no** byte- or halfword-offset table, not even in the order its own comments document (a separate
  probe with four canonical offset forms: 0 of 4). Its `add` heuristic follows the first source operand unless it
  equals the destination, so any `add` that accumulates into the offset register loses the load chain;
* for the absolute tables it takes the index register from the `lwzx` *second* operand; with the index in the
  first operand it ends up switching on the register that holds the loaded target (or on `r12` for the
  recomputed-index shape), i.e. a miscompile that traps at run time;
* the table length comes from scanning entries until an invalid one, not from the `cmplwi` bound (the first test
  table absorbed the next three, 23 entries instead of 8).

Proven by execution rather than inspection: the program recompiled with the SDK's tables traps and never finishes
in the headless runtime harness (`recomp/tests/host`); recompiled with the `[[switch_tables]]` entries of our
finder (`recomp/tools/find_switch_tables.py`, symbolic execution between the bounds check and the `bctr`, after
TTT2 Web's CC0 approach) it returns all 12 expected values. Manual tables override the detector (checked first in
`discoverBlocks`). Consequence for TTT2: the finder's output is a required input of the codegen config, and the
owner's first codegen run should include it from the start.

**Instructions (answers Q2).** Of the 66 instructions TTT2 Web added to XenonRecomp for TTT2
(`recomp/ttt2_instructions.txt`), 64 have builders in the SDK's dispatch table
(`scripts/check_instruction_coverage.py`); all 64 translate from `insn_ttt2.s`. `macchwu` and `maclhwu` (the
PowerPC 405 multiply-accumulate forms) have no builder, and the SDK's decoder interprets those opcode-4 words as
`vsldoi128`, so they are neither reported nor trapped. TTT2 Web found them only in data mistaken for code; in
ReXGlue 0.10.0 `[[invalid_instructions]]` entries are addresses consulted only when following branches
(`phase_discover.cpp`), so such words simply must not be reachable.

**Kernel/XAM surface of the runtime.** The SDK declares its exports in `src/kernel/**` in three ways:
`REX_EXPORT(__imp__Name, fn)` is a real implementation, `REX_EXPORT_STUB(__imp__Name)` a stub that logs
`<name> STUB` at warn level and returns (`_RETURN` variants return a fixed value), and
`SetVariableMapping(module, ordinal, ...)` maps a variable export into guest memory. Counted by
`scripts/gen_runtime_exports.py` on 0.10.0: **471 implemented functions (11 of them `REX_HOOK_RAW` hooks such as
`DbgPrint` and the CRT printf family), 2406 stubs, 24 mapped variables** (the
export tables know 922 kernel and 1735 XAM ordinals). Imports are resolved at compile time, so a function the
runtime neither implements nor stubs does not link into the generated host; a variable the runtime does not map is
filled with a marker (`0xD000BEEF | ordinal << 16`) and warned about at load (`xex_module.cpp`). The stubs are
therefore a free import trace: every unimplemented system call the game reaches shows up in the log as
`<name> STUB`. `ttt2-xexinfo --imports` and `ttt2-validate` classify each import of the owner's image against
these tables (`src/xex/rexglue_runtime_exports.inc`), so the report says which of TTT2's imports hit real code,
stubs or nothing before anything is run.

**TTT2's real kernel surface, from TTT2 Web.** TTT2 Web (CC0) reports itself *playable natively*: boot logos, intro
movie, title screen, menus, character select and fights run on its own runtime. That runtime implements exactly
135 kernel/XAM functions (`GUEST_IMPORT(...)` in its `runtime/kernel`), which is therefore a measured list of what
TTT2 needs up to gameplay. Against the pinned ReXGlue runtime: **130 are implemented, 5 are logging stubs, none is
missing**. The five stubs are XAM UI / stats calls (`XamShowFriendsUI`, `XamShowGamerCardUIForXUID`,
`XamShowMarketplaceDownloadItemsUI`, `XamShowMarketplaceUI`, `XamUserCreateStatsEnumerator`), which TTT2 Web
answers with "not available" results; they are the first candidates for host-side overrides if the stub counter
shows the game reaching them. This is the strongest evidence so far that the SDK runtime can carry TTT2 to the
menus without large kernel work; the owner's stub report will confirm or correct it. **Variable imports:** TTT2
Web's loader (`runtime/kernel/module.cpp`, checkout `4d6dd61`) fills seven kernel data imports by name from its
scan, `KeTimeStampBundle`, `XboxHardwareInfo`, `XboxKrnlVersion`, `XexExecutableModuleHandle`,
`ExLoadedCommandLine`, `VdGpuClockInMHz` and `VdHSIOCalibrationLock` (ordinals 173, 342, 344, 403, 430, 448,
449), so those are the ones TTT2 reads; all seven are among the 24 variable exports the pinned ReXGlue runtime
maps into guest memory (`src/xex/rexglue_runtime_exports.inc`), so no "Variable import ... not implemented"
poison is expected for them.

**Other facts measured.** `rexglue codegen` skips a project whose stamp is current ("up to date"); re-runs need
`--ignore-stamp`. `--log-level trace` prints the detector's reasoning (`detectJumpTable:` lines). The runtime
runs recompiled code without a window in tool mode (`Runtime::Setup(PPCImageConfig, {kernel_init, tool_mode})`,
`LaunchModule()`); the main guest thread's exit sets the kernel object's signal state and `exit_status` to `r3`.
Clang 18 lowers both C smoke programs' switches to value tables (no `bctr`), so C sources cannot exercise jump
tables; assembly through the SDK's bundled `powerpc-none-elf-as` can (VMX128 included; `vrlw128`/`vsel128` need
raw words because of an operand-table defect in that binutils build).

## 8b. Function boundaries on the owner's executable (codegen run of 2026-10-19)

**What the owner's run measured.** Base retail executable 0.0.0.2 (image SHA-1 `8d6389ac...`), prepared plain
by step 5c, 596 switch tables from our finder (37 with 1-byte, 28 with 2-byte, 531 with 4-byte entries; 18
dispatches with a bounds check it could not read, 1263 other `bctr` sites). `rexglue codegen` (pin `c94f5eb`)
discovered 39,374 functions (fixed point at 39,018 before the vtable and pointer scans) and gap fill 11,388 more;
49,708 sealed, 13 could not seal, and validation stopped on 14 `UnresolvedCall` errors, all of the form
`b T from S - target not in any function`.

**Mechanism, from the SDK source (identical at upstream `bd833a2`, so a pin bump does not change it).**
`phase_discover.cpp` registers new functions for `bl` targets only ("Register external call targets as new
functions (bl only, not b)"); a plain `b` to an address outside the current function is recorded as a tail call
that waits for its target to become a function by other means (`.pdata`, `bl`, RTTI vtable, lis/addi pointer,
`[functions]` config, gap fill). `phase_gapfill.cpp` splits the code into runs at `blr` and at `b` to an
already-known function, and skips a whole run when its start is already a function; the function's own discovery
stops at its first unconditional branch, so the code after it in that run is in no function. And gap fill cuts a
block that sits after a `blr` into a function of its own when the real function was never registered.
`recomp/tests/ppc/tailcall_ttt2.s` reproduces both shapes with the pinned SDK (pass 1 of the smoke test fails with
exactly two such errors).

**Each of the 14, against TTT2 Web's published table (CC0, its control-flow walk of the same image; the helper
fingerprint matches 8 of 8).** In 13 cases the branch site is a 1- or 2-instruction function in both tools
(TTT2 Web sizes 4 or 8: `b T` or an adjustor-style `addi r3,r3,N; b T`) and the target is the start of a TTT2
Web function of 0x8 to 0x68 bytes that ReXGlue never registered: 0x82CD9EA8, 0x82B16488, 0x82AAF478, 0x82AAFE00,
0x826CC0A8, 0x826CC488, 0x826CC4A8, 0x826CD708, 0x82B10A28, 0x826CC480, 0x82A4AAD0, 0x82A8EE50, 0x82ABAB40. In
the remaining case, `b 0x82959AC0` at 0x82959AF4, branch and target both lie inside TTT2 Web's function
0x82959A80+0x78: ReXGlue had no function at 0x82959A80 and its gap fill made the block at 0x82959AF0 (after the
function's `blr`) a function of its own. So: **missing boundaries (13) and one wrong range (1), both caused by an
analysis limitation of ReXGlue 0.10.0**, not by anything in our manifest, which had no explicit boundaries.

**The two tables compared on the whole image (from the owner's codegen log).** TTT2 Web lists 18,351 functions;
17,656 start where ReXGlue registered one, and of the single-block ones 14,433 agree on the size and 114 differ.
All 1,123 ReXGlue function starts that lie strictly inside a TTT2 Web function came from gap fill, none from
discovery: the published boundaries never contradict an entry point ReXGlue found itself, only its gap-fill
guesses. 695 TTT2 Web functions are not registered by ReXGlue at all (586 certainly uncovered, 28 inside a
single-block ReXGlue function, 81 undetermined), 15 KB of code in total, mostly 4, 8 and 32 bytes long. Only 13 of
them are reached by direct branches (the errors above); the rest are either dead or reached through pointers that
ReXGlue's scans do not see (vtables without RTTI: it found 352 RTTI complete object locators), which at run time
ends in the runtime's `[FATAL] Call to invalid or unregistered function at guest address X`.

**What the pipeline does about it (recomp/tools/resolve_functions.py).** Codegen runs through the resolver: each
`b T from S` error becomes a `[functions]` entry only when the instruction at S in the image is a direct branch to
T and TTT2 Web's table (applied only when ReXGlue's own helper detection in the same run matches its fingerprint)
or the image's `.pdata` agrees: T a function start gives the entry point alone (`"0xT" = {}`, ReXGlue's own
discovery finds the extent), S and T inside one reference function F give F with its size. Anything else is
reported, never guessed. On the owner's log this gives 13 entry points and 0x82959A80+0x78, no conflicts. The
boot step does the same for the runtime trap: a called address where TTT2 Web's table starts a function becomes an
entry, an address inside a function (an indirect jump into its middle, such as a jump table the finder missed) is
reported. Verified on license-free programs with the pinned SDK on Linux: the codegen errors, their resolution
and the native results (the headless harness), and the real host's trap (exit code 4), its resolution from the
host's own log and the clean run after the rebuild; the SDK workflows repeat the cycle on Linux and on Windows
(`ttt2.exe`, with the owner's boot step and triage).

## 8c. Switch dispatches made exact (session 6, before the owner's rerun)

**Why.** The owner's run left 18 dispatches with a bounds check that the finder could not read (12 after a compare,
6 after a byte load). A `bctr` without a `[[switch_tables]]` entry goes to ReXGlue's own detector
(`FunctionScanner::detect_jump_table`, used both by block discovery and by late detection while the code is
written); when it finds nothing the `bctr` becomes `REX_CALL_INDIRECT_FUNC(ctr)`, which traps at run time on a
case label ("Call to invalid or unregistered function"). Its `MissingJumpTable` validation category is never
raised, and it logs at trace level except when it finds a table load and a `lis` but cannot complete the pattern
(an error line; the owner's log has none), so its decisions do not show in a debug log. Measured on
`recomp/tests/ppc/switch_ttt2.s` by the new audit (below): of the 7 switches the detector makes there on its own,
**6 are certain miscompiles** (the `switch` tests the register holding the loaded target, so no case number ever
matches and every dispatch reaches `default: __builtin_trap()`), and 1 is consistent with its table. The previous
finder also had blind spots of its own: its register model treated the 64-bit rotates (`rld*`) and update-form
loads and stores as writing nothing, `sradi` with the high shift bit as writing rD, and it trusted facts across
code where another path can enter.

**The finder now (`recomp/tools/find_switch_tables.py`).** It symbolically executes the straight-line code that
ends at each `bctr` (up to 64 instructions, back to the last instruction that does not fall through, never across a
function start). Every register value is a constant, `(S << k) + c` for an unknown value S, a table entry
(`A + (S << k)` read with 1, 2 or 4 bytes, zero- or sign-extended, shifted, plus a base) or a second-level read of a
table entry. Instructions are modelled exactly where that is possible (add/addi/addis/subf, the mask forms of
`rlwinm`/`rldic*`, `mulli` by a power of two, `extsw`/`extsb`/`extsh`, every load and its update form); every other
instruction gives its destination registers fresh unknowns, a call (`bl`) the volatile ones, and an instruction the
write model does not know all of them. Index ranges come from a compare and the direction of the branch that leaves
on out-of-range values (`bgt`: max + 1 entries, `bge`: max; a conditional return works the same; the not-taken path
is followed), from a byte load (256 entries, then cut where the labels leave the function, as before) or from a mask
(`clrlwi`, `andi.` with 2^n - 1). Shapes this adds: the table address loaded before the bounds check, a bias folded
into the table address, `clrlslwi`/`clrldi`/`sldi`/`rldic`/`mulli` index arithmetic, `add` then a D-form load,
signed halfword offsets, two-level tables (a byte index table selecting a jump table entry), mask-bounded tables.
**Other paths into the dispatch:** branch targets from the same function, the labels of tables with a known length,
and function starts inside the analysed code are join points. A join point at or before the bounds check restarts
the analysis there (facts from before it are dropped); one after it must read the same table entry for the same
switch-register value (assuming, like the main path, that the value is a case number), or the site is reported
instead. For each dispatch it still cannot read, the JSON report names the reason and lists the disassembled code.

**How it was verified (no game code involved).** `tests/python/test_find_switch_tables.py`: instruction encoders
checked against GNU as, a small PowerPC interpreter that executes every reported table for every index value (the
switch register at the `bctr` must select the label the CTR holds) and every alternate entry path, named shape tests,
negative tests (an unmodelled instruction, a call clobbering the table register, an update load moving the table
register, a branch in the wrong direction or one that also tests CTR, the tightest of two bounds, a vtable call),
and 300 seeded random dispatches in CI. Run locally during development: 172,500 random dispatches; the final
version ran the last 50,000 with no failure. Against the previous finder on 10,000 random
dispatches: every table it found correctly is still found, it reported 242 tables that are wrong on another path
(the new finder reports those sites), and the new one finds 6,875 it missed. On `switch_ttt2.s` the 9 tables are
byte-identical to the previous finder's. `recomp/tests/ppc/switch_shapes.s` (9 new shapes plus S10, a dispatch that
another path enters with another table) goes through the real codegen in the smoke test: the finder reads the 9 and
reports S10 (the previous finder read 2: one a row too long, the other S10, wrong on that path); codegen emits the 9
as written; the native run computes `13 24 32 44 51 62 71 80 83 92 0`.

**The audit (`recomp/tools/audit_switches.py`, pipeline step 6c).** It rebuilds each instruction's address in the
generated code (ReXGlue writes one `// <mnemonic>` comment per instruction, blocks in address order, a `loc_` label
at each branch target; every `bctr` is checked against the image) and compares each `bctr` with the finder's
report: the finder's tables must be emitted as written; a switch only the SDK found is checked with the finder's
model (a certain miscompile when it tests a value read from the table or a case goes elsewhere than the table entry
for its number; suspect when another path enters; consistent when every case matches); the dispatches the finder
could not read are listed with their form (indirect call: traps if reached) and code. On the smoke programs: 0
address-mapping problems; `switch_ttt2` 9 of 9 as written, its SDK-only build 7 switches and 6 certain miscompiles;
`switch_shapes` 9 of 9 as written plus the SDK's switch at S10, called suspect. The boot step uses the finder's report
too: when the runtime traps on an address inside a function that holds an unread dispatch, it says so (the address
is then most likely a case label, not a missing function).

**Not yet measured:** the owner's executable. Expected from the owner's report (its tables' labels: only one lies
within 8 instructions of a `bctr`, at or before its compare): the 596 tables stay, some of the 18 resolve, and the
audit shows the rest, with the SDK detector's verdicts on all other `bctr` sites.

## 8d. Functions only a pointer reaches: the boot traps, predicted and registered ahead of time (session 7)

**What the owner's boots showed.** With the codegen errors resolved (8b), the whole game compiled and `ttt2.exe` ran
natively. Each run then ended on the runtime's `[FATAL] Call to invalid or unregistered function at guest address X`,
which pipeline step 9b turned into a verified entry (TTT2 Web's table lists a function starting at X) before
rebuilding: one entry per run, one rebuild per entry. By the latest run (2026-10-20) 21 entries were saved, the last
`0x82D6FC78`, and the loop had stopped at its three-run limit each time.

**The pool, from the owner's first codegen log and TTT2 Web's table.** The debug log names every function ReXGlue
discovered (`FunctionNode ... DISCOVERED`). **695** of TTT2 Web's 18,351 function starts are not among them; the 14
targets the codegen errors named are, and so is `0x82D6FC78`. They are small: 652 (94%) are 32 bytes or shorter (245
exactly 32 bytes, `0x82D6FC78` among them; 195 a single instruction; 143 two), 15 KB of code in all, the largest 320
bytes. That is the profile of virtual methods (an empty `blr`, `li r3,0; blr`, short accessors). ReXGlue's
`VTableScanner` registers the slots of RTTI vtables only, and the 337 it found are nearly all Havok classes (`hk*`,
`hcl*`, `hkcd*`); the game's own classes carry no RTTI, so a slot function that nothing calls directly and gap fill
does not cover stays unregistered until the game calls it through the vtable.

**How TTT2 Web found them.** Its `tools/find_switch_tables.py` (CC0) takes as function starts the `.pdata` entries,
the `bl` targets, every 4-aligned word in a non-code section other than `.pdata` and `.reloc` that points into code,
is no switch label, lies inside no `.pdata` function (SEH scope tables point at handler labels inside framed
functions) and is not preceded by an instruction that falls through into it; then every address another function
branches into; and, in a final sweep, every address no function covers. So most of the pool should carry the same
evidence in the owner's image: a pointer in its data.

**The ahead-of-time pass (`resolve_functions.py codegen --indirect`, pipeline step 6b, `build-host.ps1`).** After a
successful codegen it reads the registered functions from the generated `*_register.cpp` and decides for every
reference start missing there. One is registered only when a 4-aligned word in a data section (not code, `.pdata` or
`.reloc`) holds its address (code is never searched: an instruction can equal a code address, `0x82D6FC78` itself
decodes as `lwz r22,-904(r22)`), the image has code there, the instruction before it cannot fall through into it, it
lies inside no `.pdata` function, it is no case label of the finder's tables, its reference extent is plain code (no
zero word) ending in a branch or return, and ReXGlue registered nothing inside that extent. A start no data word
points at (TTT2 Web's sweep finds those too) is never registered this way: the boot step still adds one the game
calls, with the call as its evidence. Jump tables cannot pass for pointers: the owner's 531 four-byte (absolute)
tables all sit in `.text` right after their `bctr`, and the 65 tables in data sections hold 1- or 2-byte offsets.
The verified ones are compiled as one batch. If codegen then fails in a way the error resolution cannot fix, the
batch entries the errors point into are taken out and listed in the output file (later runs skip them); if that does
not help, the whole batch is rolled back to the entries that compiled before and codegen reruns on those. Every
entry of the output file must then be registered in the generated code; the report names any that is not.
`--drop-indirect` (the pipeline's `-NoIndirect`) removes the ahead-of-time entries and keeps every other one.

**What registering does to the function that branches there (measured with the pinned SDK).** A pointer-only
function that a branch of a registered function reaches is not loose code but an internal label of that function
(ReXGlue follows the branch into it). Registering it splits the function: an unconditional `b` becomes a tail call
(`sub_X(ctx, base); return;`), a conditional branch a conditional tail call (`if (ctx.cr0.eq) { sub_X(ctx, base);
return; }`). `recomp/tests/ppc/pointer_ttt2.s` has both shapes, with the pointers in `.rdata` and in `.data`, plus
three negative controls the pass must refuse (no pointer; a `bl` falling through into the address; a label inside a
`.pdata` function). Codegen alone: 6 functions, and the first call through a pointer traps at `0x820100E4` with the
same `[FATAL]` line. With `--indirect`: both functions registered, the three controls refused for their own reasons,
8 functions, and the native run computes `50 6 57 205 204 1004 62`. `tailcall_ttt2.s` from scratch: the 2 entries its
codegen errors call for, then its pointer-called leaf ahead of time; the first run passes without the trap, log and
rebuild its boot cycle needed before.

**The boot loop (pipeline steps 9b and 9c).** It repeats by itself, up to `-BootRounds` runs (default 10, never more
than 50), and stops with the reason in the report, keeping every verified entry: a run that ends some other way; a run
whose log gives no new verified entry; a trap at an address the loop already added, or one the entries the host was
built with already list (the entry did not take effect); `-BootMinutes` (default 480) before a rebuild that would
exceed it; less than `-MinFreeGb` (default 5) GB free before a rebuild; a failed rebuild. The runs after a rebuild
close the game after 300 s even with `-BootTimeout 0`, so the loop never waits for a person. Step 9c lists every
run (how it ended, what its log added). The loop reads the triage and the resolver's summary lines, not their JSON.
`tests/powershell/test_boot_repair.ps1` runs the loop's own code with scripted boots and rebuilds (CI: PowerShell 7
and Windows PowerShell 5.1); the Windows workflow runs the whole pipeline on a test disc carrying `pointer_ttt2`
(`tests/tools/make_test_iso.cpp`): with `-NoIndirect` three runs and two automatic rebuilds, with the default one run.

**Not yet measured:** the owner's image. The resolve report's ahead-of-time section will say how many of the pool have
a pointer in data, how many each check refuses, and whether codegen accepts the batch.

## 8e. The black screen: the one error, the disc sector query, and a boot monitor (session 8)

**What the owner's run showed (2026-10-20, checkout `0d1d012`).** Everything up to native execution worked: the
disc identified, the executable prepared, 583 switch tables, 348 verified function entries (327 ahead of time),
codegen with no unresolved call, `ttt2.exe` built, all 78 game files extracted. The game created 28 guest threads,
reached no kernel stub, and showed a black window (no Bandai Namco logo) until the pipeline closed it after about
180 s. The watchdog reported a possible stall after 15 s. The log held one error, `ResolvePath(\Device) failed -
device not found`; the host exited 0 on the close request (not a natural exit). The boot loop stopped after one run
because no unregistered function was called.

**What the error means (read from the pinned SDK, then reproduced).** `VirtualFileSystem::OpenFile` resolves the
parent of the path it opens first; `ResolvePath` logs this error when no mount matches. ReXGlue mounts the game folder
at `\Device\Harddisk0\Partition1` (behind `game:` and `d:`), `update:` at `\Device\Harddisk0\PartitionUpdate` and a
null device at `\Device\Harddisk0` for the raw partitions; nothing is mounted at `\Device` itself and there is no disc
device (`\Device\Cdrom0`). So the error is printed exactly when the game opens a bare device path `\Device\<name>`
(nothing after the device name): the parent `\Device` does not resolve, and `NtCreateFile` fails with
`STATUS_NO_SUCH_FILE`. `recomp/tests/ppc/io_ttt2.s` opens `\Device\Cdrom0` through a real import table and produces
the identical line. The path itself is in the owner's log as the runtime's
`[NtCreateFile] FAILED: path='...' -> 0xc000000f` warning, which the triage did not list.

**Why a game opens its disc device.** `NtQueryInformationFile` with `FileSectorInformation` asks for the disc sector a
file starts at. ReXGlue 0.10.0 answers it with success and a made-up number (a hash of the path; Xenia's note: some
titles only use it to tell files apart), logged at debug level only. TTT2 Web (CC0), which runs this game to menus
and fights, answers it as unsupported (`STATUS_INVALID_INFO_CLASS`) with the comment that TTT2's loader reads its
archives by raw sector through the disc device when the query works and falls back to ordinary file reads when it
does not; its `TTT2_FAKE_SECTORS=1` switch, which returns a hash instead, exists to try the other path. Two
numbering notes: TTT2 Web names class 27 the sector class, while the Xbox 360 numbering ReXGlue (and Xenia) use
puts `XFileSectorInformation` at 26 and `XFileXctdCompressionInformation` at 27 (`FileNetworkOpenInformation` is 34
in both). TTT2 Web fails both 26 (its default case, `STATUS_NOT_IMPLEMENTED`) and 27; ReXGlue succeeds 26 and fails 27
with an error line ("XFileXctdCompressionInformation unimplemented") that the owner's log does not contain, so the
game did not ask for class 27 before it stopped.

**The working hypothesis (not yet confirmed on the real game).** ReXGlue's made-up sector for class 26 sends TTT2's
loader down the raw-sector path; the loader opens `\Device\Cdrom0`, the open fails (the one error), and loading never
completes, so no frame is ever presented. It explains every fact of the report, and TTT2 Web's working configuration
is the same with the one difference this changes. What would contradict it: the device opened although the sector
query is answered as unsupported, or opened before any sector query.

**The change (`recomp/src/io_overrides.cpp`).** The host answers `FileSectorInformation` (class 26) on a file handle as
unsupported, `STATUS_INVALID_INFO_CLASS`, as TTT2 Web does, and logs the first query with its file. Every other class
goes to the runtime. `TTT2_SECTOR_QUERY=runtime` restores the runtime's answer for comparison. The host does not map
a device at `\Device\Cdrom0` yet: with the SDK's `OpenFile`, a bare device path never opens (its parent `\Device`
has no mount), so a disc device needs a change in the runtime's file system rather than another symbolic link, and
nothing shows the game needs the device once the sector query fails. If the next run still opens it, that is the
next step (TTT2 Web maps `\Device\Cdrom0` to the game folder).

**How the host changes one case of a runtime export.** The recompiled code calls each import as the C symbol
`__imp__<Name>`, bound at link time, so a definition in the host replaces the runtime's (as for the five XAM
overrides of `kernel_overrides.cpp`). A wrapper still needs the runtime's own function for everything else:
`recomp/src/runtime_exports.cpp` finds it by name, with `dlsym(RTLD_NEXT)` on Linux and `GetProcAddress` over the
loaded modules other than the executable on Windows. That works because the runtime library exports every
`__imp__` symbol (2,866 in the pinned build: the generated code links against them). `Ttt2App` checks at start-up
that the original of every wrapper is found, and the harness, the Linux host and the Windows host tests exercise it.

**Upstream.** The faithful fix belongs in ReXGlue: answer `FileSectorInformation` from the device a file is on - the
real sector for a file of a mounted disc image (`DiscImageDevice` knows it), unsupported for a host folder - and let
`OpenFile` open a device root, so a disc device at `\Device\Cdrom0` is possible. Until the SDK has that, the host's
answer is the compatible one; it is one `case` to drop when the pin moves.

**The boot monitor (`recomp/src/boot_monitor.cpp`, `wait_overrides.cpp`, `milestone_overrides.cpp`).** 47 wrapped
exports record what each guest thread asks the kernel for. A wrapper costs a thread-local lookup and a few relaxed
atomic stores; nothing in the hot path takes a lock or reads a clock. The watchdog (every 5 s) and the dumps report:

* *Boot progress* (one `ttt2: activity:` line per tick): files opened and failed (bare device paths counted apart),
  bytes and reads, sector queries, threads created, GPU set-up (engines, ring buffer, graphics interrupt callback),
  the GPU emulation's vblank counter and ring write pointer (read from the Xenos plugin's graphics system), frames
  presented (`VdSwap` calls: none means nothing was ever presented, not black frames), audio clients and frames,
  controller polls, traced calls. Files opened, bytes read, frames and threads created now count as progress for
  the stall detector, alongside the thread census and stub calls.
* *File access*: each distinct path opened, each distinct failure with its status, API, thread and guest call site
  (the first 64 of each), missing-file attribute queries, read failures, device controls, and an explanation the
  first time a bare device path fails.
* *Milestones*: the first call of each GPU set-up export and every interrupt callback, ring buffer and write-back
  registration with its arguments; the first frame; audio client registration and the first audio frame; the first
  controller poll; system dialogs (sign-in, message box, keyboard, device selector) and content creation with their
  results; every thread the game creates with its start address.
* *Per thread, in each stall dump* (after 15 s without progress, again after 60 s, and when the window closes): the
  blocking call it is in (`KeWaitForSingleObject`, `KeWaitForMultipleObjects`, the `Nt*` waits,
  `NtSignalAndWaitForSingleObjectEx`, `KeDelayExecutionThread`, `RtlEnterCriticalSection`) with the objects named
  from guest memory (event or semaphore state, the owning thread of a mutant or critical section), its timeout, how
  long it has been seen in it and the guest address it was called from; its traced calls (in the last tick, in
  total, how long it has been quiet, the last one); and its guest call stack, read from its own stack (the link
  register, then the return address the Xbox 360 ABI saves at -8 from each caller's frame), each address named by
  the recompiled function holding it. Process-wide: the busiest traced calls of the last tick and the last 12 file
  events.

`recomp/tools/triage_boot_log.py` reads all of it: a boot-progress table, file access, milestones, threads created,
per-thread details of every dump, and recommendations (no frame presented and where the boot stopped; a bare device
opened with or without the sector query answered; deadlock cycles and wait chains through lock owners; threads
waiting without a timeout on objects nobody signals).

**Measured with license-free programs.** `io_ttt2.s` through the harness and both hosts: the sector query returns
`0xC0000003` (the runtime's success with `TTT2_SECTOR_QUERY=runtime`), open, size, read and close by the runtime
through the wrappers, `\Device\Cdrom0` fails with `0xC000000F` and the runtime's "ResolvePath(\Device)" line, a
missing file's attributes fail; all ten values checked. `wait_ttt2.s` (the main thread waits on a critical section
a worker holds; the worker waits on an event nobody sets): the dump names the critical section with its owner, the
unsignaled event, both call sites and stacks and the worker's start address, and the triage reports the wait chain.

## 8f. The black screen, second run: a kernel clock that never moves, `\Device\Image`, and what the frames hold (session 9)

**What the owner's run showed (2026-10-20, `main` at `33f43c8`, the PR #16 host).** Everything up to native execution
as before (348 verified function entries, codegen clean, 28 guest threads). The GPU was set up and the game presented
frames: 10,760 `VdSwap` calls, 21,548 vblanks and 10,762 controller polls in 180 s, all black. It read 2 files, 0.0 MB
in all. One bare device was opened, `NtCreateFile("\Device\Image")` from `0x82BCC930` in `sub_82BCC880`, failing with
`0xC000000F`; no disc sector query reached the host, so the sector hypothesis of 8e does not apply to this run (the
game never asked). Thread 6 was running while threads 12 and 26 waited on a critical section it owned (recursion 390),
and thread 14 waited on a lock thread 12 owned. Thread 19 made about 339.5 million traced kernel calls (about 1.9
million a second). The pipeline closed the window after 180 s.

**The strongest lead: the kernel clock the game reads from memory stands still.** `KeTimeStampBundle` (xboxkrnl data
export 0xAD) is 24 bytes the game reads without a kernel call: InterruptTime (u64, 100 ns units since boot) at +0,
SystemTime (u64, FILETIME) at +8, TickCount (u32, milliseconds) at +16. The pinned SDK allocates it and patches the
game's import slot, but its 1 ms timer refreshes only TickCount: InterruptTime and SystemTime stay 0 for the whole run
(`src/kernel/xboxkrnl/xboxkrnl_module.cpp`). Both reference ports refresh all three every millisecond. TTT2Recomp
measured why it matters for this game: code reads the bundle through its import slot (its `sub_82BEF480`, NACE's
clock), and with the fields at 0 "those clocks read 0 and never advanced" - a 1 s timeout waited forever. TTT2 Web
documents a game thread (`sub_82ABFDE8`) that "sets an auto-reset event and SignalAndWaits on it until a 64-bit counter
changes (~8 M kernel calls/s)": on a counter that never changes that loop never ends, which matches thread 19 (millions
of traced calls a second, without a single blocking wait). Clocks that never advance also stall every timed wait and
time-sliced loader step built on them, which fits a loader that read 2 files in 180 s. Not yet confirmed on the real
game; the next run shows it (the loader's reads, the spinning thread).

**The change (`recomp/src/kernel_clock.cpp`).** A host timer refreshes InterruptTime and SystemTime every millisecond
from the runtime's own guest clock (`rex::chrono::Clock`): SystemTime as `KeQuerySystemTime` reports it, InterruptTime
as the time since the guest time base (the same offset the runtime derives TickCount from), never going backwards.
TickCount stays the runtime's (two writers could make it step back a millisecond). The bundle is found through the
export resolver (ordinal 0xAD) after the runtime is set up, in the game host and in the test harness alike.
`TTT2_KERNEL_CLOCK=runtime` keeps the runtime's behaviour. The activity line reports the clock (`kernel clock 12.3 s
(tick count 12.3 s)`). Upstream, the SDK's timer should write all three fields.

**`\Device\Image`.** The game opens the device itself (nothing after the name), so the SDK's `OpenFile` fails to
resolve the parent `\Device` and logs "ResolvePath(\Device) failed" (8e). Neither reference port maps such a device,
and both run the game to its menus: TTT2 Web answers any missing name with `STATUS_OBJECT_NAME_NOT_FOUND`
(`0xC0000034`), I/O status `{0xC0000034, 0}` and the caller's handle variable untouched; TTT2Recomp strips `\Device\X\`
prefixes, finds nothing for `\Device\Image` and answers `0xC0000034` for opens and attribute queries alike. That is
also the console object manager's answer for a name missing from `\Device`. The runtime instead answers
`STATUS_NO_SUCH_FILE` (`0xC000000F`), writes `INVALID_HANDLE_VALUE` into the handle variable and `FILE_DOES_NOT_EXIST`
(5) into the I/O status block. What the game wants from the device (a disc image, raw sectors, a directory) is not
visible in the logs; the call site handles the failure in both working ports. So the host maps nothing there - a
device that pretends to exist would be a guess the game then builds on - and instead answers a missing name the way the
console and both ports do (`recomp/src/io_overrides.cpp`): for `NtCreateFile` and `NtOpenFile`, `0xC0000034` with I/O
status information 0, and the handle variable left as it was on any failed open; for `NtQueryFullAttributesFile`,
`0xC0000034`. Only the runtime's not-found status is translated; every other answer is the runtime's.
`TTT2_NOT_FOUND_STATUS=runtime` keeps the runtime's answer.

**What the frames hold.** 10,760 black frames can be frames the game draws nothing into (its own black screen while it
waits for data or a timer) or frames it draws that come out black (the GPU emulation). The host now reads each frame's
command stream right after `VdSwap` (`recomp/src/frame_monitor.cpp`, the SDK-free parser `recomp/src/pm4_stats.cpp`):
the stretch of the primary ring buffer between the previous swap and this one (wrapping at its end), and the indirect
buffers it names, from guest physical memory (only allocated pages). The parser follows the packets as the SDK's
command processor executes them (a zero word skipped, predicated packets skipped while bin select and mask do not
overlap, `SET_CONSTANT` type 4 and `SET_CONSTANT2` writing registers) and counts: draws by the EDRAM mode in force
(`RB_MODECONTROL`: color+depth and depth-only render, copy is a resolve, off does nothing), resolves that clear
(`RB_COPY_CONTROL` bits 8-9), render-target changes (`RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`), indirect
buffers, swaps, and packets the SDK does not execute (its "Unimplemented GPU OPCODE" cases, e.g. binned draws), by
opcode. The first three frames, the first frame with draws, the first with skipped packets and every front-buffer
change (size, format, address, from `VdSwap`'s arguments) are logged; the activity line carries the totals. A pixel
check is opt-in (`TTT2_PIXEL_CHECK=1`): the GPU emulation renders on the host GPU and copies results back to guest
memory only with `readback_resolve` (default `none`), so the check sets it to `fast` (the frame arrives one frame
late) and samples the presented front buffer: distinct values and the most common one, nothing else is kept.

**Thread synchronisation.** The pinned SDK's critical sections (`src/kernel/xboxkrnl/xboxkrnl_rtl.cpp`) follow NT's
algorithm (a lock count, a recursion count, the owner, the section's own synchronization event for waiters); nothing
there miscounts, so a recursion count of 390 is the game's own nesting at that moment. The host does not release
locks or signal anything. It now shows what the dumps could not: per thread, the calls of the last tick by export, and
a `busy:` line for a thread making 50,000 or more traced calls a second (with its rate, busiest call and call site);
the triage names spinning threads (also from the old logs' "N in the last tick", at 250,000 per 5 s tick), ties them to
the kernel clock (an old host, the refresh turned off, or a clock that advances so the loop waits for something
else), and reports a critical section whose recursion count changes between dumps under the same owner (entered more
often than left).

**A shutdown race, found by the new test.** When the window closes, `ReXApp::OnClosing` terminates the title, which
ends the main guest thread; `ReXApp`'s module thread then calls `OnGuestThreadExit` while the close path is still
running. With a busy thread delaying termination, the host's handler logged "guest program ended" - which the triage
reads as a clean exit. The host now notes the close request and logs `main guest thread ended by the window close`
instead, leaving the exit to the close path.

**Measured with license-free programs.** `clock_ttt2.s` reads the bundle through a variable import slot (a type 0
record with no thunk): with the host's refresh, InterruptTime and SystemTime advance over a 50 ms sleep, agree with
TickCount and `KeQuerySystemTime`, and a thread that signals its own event and waits on it until InterruptTime changes
leaves its loop; with `TTT2_KERNEL_CLOCK=runtime` they stay 0 and the loop runs into its 1,000,000-round cap (about
5 million calls a second on the build machine). `io_ttt2.s` now opens `\Device\Image`, reads the handle variable and
the I/O status block back, and opens a missing file with `NtOpenFile`: 15 values, checked in both modes. `gpu_ttt2.s`
writes three frames of PM4 packets into a 4 KB ring and presents each; the host's counts match exactly (frame 1: 3
draws, 1 clearing resolve, 1 render-target change, 1 indirect buffer, 1 skipped packet, 10 packets, 26 words; frame
2: 1 draw with the EDRAM mode off; frame 3, across the end of the ring: 2 draws in 872 words); the pixel check reads
its 16x16 front buffer (75% `0x11223344`). `wait_ttt2.s` gained a third thread that spins like `sub_82ABFDE8`: the
dump flags it at about 5 million calls a second with `NtSignalAndWaitForSingleObjectEx` as its call, and the triage
names it.

## 9. Sources

* https://github.com/Ekspym/Tekken6-native-online and https://github.com/Ekspym/tekken6-native-online-source
* https://github.com/mahmoud-mess/tekken6-recomp
* https://github.com/shihabal3amri/tekken-tag-tournament-2-pc (and its releases page)
* https://github.com/monty488/tekkenwebtournament2 (CC0-1.0)
* https://github.com/rexglue/rexglue-sdk (wiki: Getting Started, CLI Commands, CLI Configuration File, Codegen
  Pipeline Overview, Runtime Architecture Overview; releases)
* https://github.com/hedge-dev/XenonRecomp, https://github.com/hedge-dev/XenosRecomp
* https://github.com/pond3r/ggpo
* https://github.com/xenia-project/xenia
* https://steamdeckhq.com/news/we-may-see-more-xbox-360-native-pc-ports/ (community discussion of ReXGlue)
