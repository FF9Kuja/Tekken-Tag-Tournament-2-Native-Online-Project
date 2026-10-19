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
  `a942090b7c293d0ce63346ed8a2974448135d0dd`; TU3-patched decrypted image SHA-1 `66adbc1a7744ec840a3ae8e51f8aeb677140d789`
  (both **reported**); XGD3 redump ISO game partition at `0x2080000` (XGD2 `0xFD90000`); 45 top-level disc files
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
menus without large kernel work; the owner's stub report will confirm or correct it.

**Other facts measured.** `rexglue codegen` skips a project whose stamp is current ("up to date"); re-runs need
`--ignore-stamp`. `--log-level trace` prints the detector's reasoning (`detectJumpTable:` lines). The runtime
runs recompiled code without a window in tool mode (`Runtime::Setup(PPCImageConfig, {kernel_init, tool_mode})`,
`LaunchModule()`); the main guest thread's exit sets the kernel object's signal state and `exit_status` to `r3`.
Clang 18 lowers both C smoke programs' switches to value tables (no `bctr`), so C sources cannot exercise jump
tables; assembly through the SDK's bundled `powerpc-none-elf-as` can (VMX128 included; `vrlw128`/`vsel128` need
raw words because of an operand-table defect in that binutils build).

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
