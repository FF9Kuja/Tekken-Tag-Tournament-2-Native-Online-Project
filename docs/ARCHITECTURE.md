# Architecture

*Selected in Milestone 1 (2026-10-18). This describes the target system and the reasoning; most of it is not
built yet. Implemented parts are marked **[implemented]**; everything else is planned. Decision record:
`docs/decisions/0001-use-rexglue-sdk.md`.*

## 1. Goals and constraints

* A native Windows 10/11 x64 executable that runs Tekken Tag Tournament 2 (Xbox 360, TU3) without an emulator.
* GGPO-style rollback netcode without changing the game's mechanics.
* **No game content in this repository or in any release.** The player builds the game on their own PC from their
  own disc image and Title Update 3 (the model used by Tekken 6 Native Online and TTT2Recomp).
* Everything that can be validated without Windows or game files (tooling, parsers, codegen on test inputs,
  determinism harnesses) is validated on Linux CI; Windows-only steps are documented and scripted.

## 2. System overview

```
                         player's own files (never redistributed)
        ┌──────────────────────┐        ┌────────────────────────────┐
        │  TTT2 disc image     │        │  Title Update 3 (STFS)     │
        │  (.iso, XDVDFS)      │        │  Default.xexp + data100.*  │
        └──────────┬───────────┘        └─────────────┬──────────────┘
                   │                                   │
   ┌───────────────▼───────────────────────────────────▼──────────────┐
   │ (A) Ingestion & validation  [tools/, launcher]                    │
   │  xdvdfs reader → Default.xex + data files      stfs reader → xexp │
   │  xex2 header parse [implemented] → identify title/version/hashes  │
   │  xex decrypt + decompress + XEXP delta patch → patched image      │
   └───────────────┬───────────────────────────────────────────────────┘
                   │ patched Default.xex (local only)
   ┌───────────────▼───────────────────────────────────────────────────┐
   │ (B) Recompilation  [recomp/]                                      │
   │  rexglue codegen ttt2_manifest.toml                               │
   │   + ttt2_config.toml: functions, switch tables, hooks, rexcrt     │
   │   + post-codegen patch tools (netplay instrumentation)            │
   │  → generated/default/recomp.N.cpp (local only)                    │
   └───────────────┬───────────────────────────────────────────────────┘
                   │ C++ (compiled on the player's PC with bundled clang)
   ┌───────────────▼───────────────────────────────────────────────────┐
   │ (C) Native host  [src/host]            ttt2.exe                   │
   │  ReXGlue runtime: kernel/XAM HLE, guest memory, VFS, threads,     │
   │  Xenos GPU → D3D12, XMA audio, SDL3/XInput input, ImGui overlay   │
   │  Ttt2App (rex::ReXApp subclass): paths, patches, hooks, settings  │
   │  (D) Netplay layer: GGPO, frame/input/seed hooks, snapshots,      │
   │      audio boundary, determinism diagnostics                      │
   └───────────────┬───────────────────────────────────────────────────┘
                   │ UDP P2P (GGPO)            ▲ status/commands
   ┌───────────────▼───────────────────────────┴───────────────────────┐
   │ (E) Online services: lobby/matchmaking service, NAT traversal     │
   │ (F) Launcher: validation, build pipeline, settings, updates       │
   └───────────────────────────────────────────────────────────────────┘
```

## 3. Components

### (A) Game-file ingestion and validation - `src/core`, `src/xex`, later `src/stfs`, `src/xdvdfs`

Own code (clean-room, from public format documentation), unit-tested with synthetic images.

* **[implemented]** `ttt2::xex` XEX2 header parser and `ttt2-xexinfo`: title ID, version/base version, media ID,
  entry point, image base, encryption/compression, import and static libraries, security info, delta-patch
  descriptor (source/target version) for `.xexp` files; SHA-1/SHA-256 of the file; match against known hashes.
* **[implemented]** XDVDFS reader + `ttt2-iso`, STFS reader + `ttt2-stfs`, XEX basefile decryption (AES-128-CBC,
  retail key) and basic decompression, the title-update pre-check (`patch_check.hpp`), import resolution against
  the kernel/XAM export tables and the save/restore helper scan (`imports.hpp`), `ttt2-validate` (the whole
  check in one command), `ttt2-xexpatch` on XenonUtils' MIT `XexPatcher` (Clang-only). TTT2's retail executable
  is basic-compressed (confirmed by TTT2Recomp's unpack tool), so LZX decompression is needed only for the data
  containers, later.
* Output: a verified set of local files (`game/`, `update/`) and a patched `Default.xex` whose hash identifies the
  exact build the generated code corresponds to.

### (B) Recompilation - `recomp/`

* **ReXGlue SDK** (pinned commit, BSD-3) `rexglue codegen` with `recomp/ttt2_manifest.toml` and
  `recomp/ttt2_config.toml`. The config holds only addresses and names (no game bytes) and may be committed.
* Config content expected (from the T6 and TTT2 experiences): explicit `[functions]` boundaries for leaf functions
  split by jump tables, `[[switch_tables]]` for the 526-535 TTT2 tables - **[implemented]** generated by
  `recomp/tools/find_switch_tables.py` (symbolic execution; required, because the SDK's detector misses offset
  tables and switches on wrong registers for several shapes, `docs/RESEARCH.md` 8a) - `[[invalid_instructions]]`
  for data-as-code (addresses in ReXGlue), `[rexcrt]` for `memcpy`/`memset`/heap functions, `[[midasm_hook]]` for
  the netplay hooks, and all `*_as_local` optimizations OFF until the game boots (then enabled one at a time with
  determinism tests). `recomp/tools/codegen_report.py` **[implemented]** turns every codegen run into a report.
* **Post-codegen patch tools** (`recomp/tools/*.py`): rewrite the generated C++ to add dirty-page tracking on guest
  stores and the sound-library call boundary. Pure text transforms over local files; the tools are ours.
* **Cross-check path**: XenonAnalyse / XenonRecomp (MIT) on the same image to compare function boundaries and
  jump tables, as the T6 projects did.
* Codegen runs on Linux (CI can exercise it on license-free PowerPC ELF test inputs, and developers with game files
  can run it in WSL/Linux or Windows). The generated code is never committed (`.gitignore` + hygiene check).

### (C) Native host - `src/host`

* `Ttt2App : rex::ReXApp` (`recomp/src/ttt2_app.cpp`). Overrides **[implemented]**: `OnConfigurePaths`
  (game/update/user roots, `update/` sibling auto-detected), `OnPreSetup` (backend log), `OnPostLoadXexImage`
  (identification against title `4E4D0859` / TU3 0.0.3.2, import summary), `OnPostLaunchModule` /
  `OnGuestThreadExit` (guest-thread watchdog census). Planned: data patches, hook registration, `OnCreateDialogs`
  (overlay). A headless harness (`recomp/tests/host`) runs recompiled programs through the same runtime without
  a window, which is how recompiled code was executed natively for the first time (session 3).
* Graphics: SDK D3D12 Xenos backend (Vulkan as a secondary option). Internal resolution and present timing
  handled through SDK mechanisms plus game-specific hooks (as T6 NO does for its 1024x576 targets).
* Audio: SDK XMA decoder + SDL3 audio driver; a deterministic stepping mode for netplay (D).
* Input: SDK SDL3 (Xbox + PlayStation controllers, keyboard) and XInput; netplay feeds `XamInputGetState`.
* Saves/settings: SDK content manager under the user data root; no Program Files writes.
* TTT2-specific kernel/XAM behaviour fixes live as SDK patches (`third_party/rexglue-patches/`) or overrides in the
  app, documented per fix.

### (D) Netplay layer - `src/netplay` (not before Milestone 6)

Design modelled on the Tekken 6 Native Online approach (knowledge only; our implementation):

* **Mode:** the game runs its offline Versus mode; GGPO provides the inputs of both players.
* **Frame boundary hook** at the head of the game's main loop (the per-frame task-tree step): advance the previous
  frame, `ggpo_idle`, add local input, synchronize inputs, then run the frame. A rollback restores a snapshot and
  re-runs the per-frame step N times.
* **Input hook** where both players' input entries are built, before the input manager consumes them. The original
  2-frame offline input FIFO is neutralised (GGPO's delay replaces it).
* **Seed hooks** on the game's LCG `srand` and the CRT `srand(time)` to use an agreed seed; cosmetic RNG consumers
  isolated.
* **Snapshots:** XEX data/bss + game heaps for TTT2 (ranges to be measured), with dirty-page maps written by every
  guest store (post-codegen patch), taken after GPU idle and worker idle; ring of recent frames.
* **Audio boundary:** the sound library is not rolled back; calls into it are recorded on live frames and replayed
  on re-simulated frames; the audio engine is stepped once per simulated frame.
* **Determinism tooling first:** input recording/replay, per-range state hashes per frame, save/restore self-test,
  two-instance local sync test (`loopback`) - all before any internet play.
* **Transport:** UDP peer-to-peer (GGPO's), delay/prediction configurable, latency and rollback statistics in the
  overlay.

### (E) Online services - `services/` (Milestone 7)

Start free: direct P2P with UPnP port mapping and UDP hole punching through a tiny rendezvous service; private
rooms by code; lobbies and matchmaking on a small self-hosted service (TLS, pinned certificate, rate limiting,
version pairing by exe hash). A relay is a later option only if NAT failure rates demand it.

### (F) Launcher - `launcher/` (Milestone 8)

Windows application: pick ISO + TU3, validate (hashes from (A)), run the build pipeline (B) with a bundled
llvm-mingw clang++ and prebuilt host libraries (reproducible flags so peers can be matched by exe hash),
incremental rebuilds after updates, controller/keyboard configuration, network settings/diagnostics, self-update,
clear error messages with log collection. UI toolkit is an open choice (Qt is LGPL and heavy; a Win32/ImGui or
WebView2 UI are lighter alternatives) - decided in Milestone 8.

## 4. Memory and ABI facts carried into the design

* Guest address space: a 4 GiB reservation so `base + guest_address` is always valid (both references add a
  committed zero tail / null page handling). Image base `0x82000000` for retail titles; TTT2's exact load address,
  image size and TLS come from its XEX headers (readable with `ttt2-xexinfo`).
* All guest data is big-endian; `ttt2::be<T>` and the `load_be*` helpers are the only swap points
  **[implemented]**.
* Recompiled functions have the signature `void sub_XXXXXXXX(PPCContext& ctx, uint8_t* base)`; kernel imports are
  `__imp__Name` functions resolved at link time; indirect calls go through a lookup table in guest memory.
* Floating point: generated code is compiled with `-ffp-contract=off` and a fixed `-march` (x86-64-v2) so every
  player's build computes identically; FMA/AVX variants must never be mixed between peers.

## 5. Repository layout (current and planned)

```
CMakeLists.txt, CMakePresets.json   first-party tooling build (Linux + Windows presets)   [implemented]
cmake/                              warning policy, version header                          [implemented]
src/core/                           endian, bounds-checked reader, SHA-1/SHA-256, file I/O  [implemented]
src/xex/                            XEX2 header parser, known titles                        [implemented]
src/tools/xexinfo/                  ttt2-xexinfo CLI                                        [implemented]
tests/                              doctest unit tests (311 assertions)                     [implemented]
third_party/doctest/                test framework (MIT), header fetched at configure time    [implemented]
scripts/                            build/test scripts, hygiene check, git hook             [implemented]
.github/workflows/ci.yml            Linux (clang, gcc) + Windows (MSVC, clang-cl) CI        [implemented]
docs/                               research, architecture, roadmap, dependencies, legal    [implemented]
src/xdvdfs/, src/stfs/              disc image and title-update package readers              [implemented]
src/tools/{iso,stfs,xexpatch,validate}/  the remaining command-line tools                    [implemented]
recomp/                             ReXGlue project: manifest, config, tools/, tests/ppc, tests/host, src/ttt2_app  [implemented, M3/M4 in progress]
third_party/rexglue-sdk/            SDK pin + fetch/build scripts (checkout is git-ignored)  [implemented]
third_party/xenonrecomp/            XenonRecomp pin (XexPatcher, export tables)             [implemented]
src/netplay/                        rollback layer                                          Milestone 6
services/                           lobby / rendezvous                                      Milestone 7
launcher/                           Windows launcher                                        Milestone 8
```

## 6. Principles

1. **Determinism before features.** Every runtime change that could affect simulation order is measured with the
   hash/replay tooling before it ships.
2. **Instrument through codegen, not binary patching.** Hooks and dirty tracking are config and text transforms
   over regenerated code, reproducible from the player's files.
3. **Reproducible player builds.** Same compiler, flags and prebuilt libraries for everyone; the exe hash is the
   compatibility key online.
4. **Keep the SDK upstreamable.** SDK changes live as discrete patches with a reason each; prefer app-side hooks.
5. **Honest status.** Stubs and prototypes are labelled as such in code and in `docs/STATUS.md`.
