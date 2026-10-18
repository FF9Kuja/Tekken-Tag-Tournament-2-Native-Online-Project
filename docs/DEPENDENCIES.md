# Dependencies

All third-party code is listed here with its license and how it enters the build. Nothing from Bandai Namco or
Microsoft is a dependency: game content is supplied by the player at install time and never redistributed.

## Used now (Milestone 2)

| Dependency | Version | License | How | Purpose |
|---|---|---|---|---|
| doctest | 2.4.11 | MIT | downloaded at configure time with a pinned SHA-256 (`third_party/doctest/CMakeLists.txt`; license kept in-tree) | unit tests only |
| CMake | >= 3.25 (3.28 tested) | BSD-3 | developer tool | build system |
| Ninja | 1.11 tested | Apache-2.0 | developer tool | build backend (Linux, clang-cl presets) |
| Clang / LLD | 18.1 tested | Apache-2.0 with LLVM exception | developer tool | compiler (also emits PowerPC test inputs) |
| GCC | 13.3 tested | GPL-3.0 with runtime exception | developer tool | second compiler for portability checks |
| MSVC / Windows SDK | VS 2022 | proprietary (free Community / Build Tools) | developer tool | Windows builds, CI |
| Python | 3.10+ | PSF | developer tool | scripts (hygiene check, later patch tools) |
| GitHub Actions: actions/checkout, ilammy/msvc-dev-cmd | v4 / v1 | MIT | CI | checkout, MSVC environment |

## Planned (Milestone 3+)

| Dependency | License | Planned use | Notes |
|---|---|---|---|
| ReXGlue SDK (rexglue/rexglue-sdk) | BSD-3 (portions Xenia, BSD-3) | codegen + runtime | pin a commit; carry patches in `third_party/rexglue-patches/`; its own submodules: FFmpeg (LGPL-2.1+ build, XMA only), SDL3 (zlib), Dear ImGui (MIT), spdlog/fmt (MIT), tomlplusplus (MIT), inja (MIT), xxHash (BSD-2), simde (MIT), libmspack (LGPL-2.1), Vulkan headers/loader (Apache-2.0), glslang/SPIRV-Tools (Apache-2.0/BSD), cli11 (BSD-3), o1heap (MIT), utfcpp (Boost), tracy (BSD-3, optional), catch2 (Boost, tests) |
| XenonRecomp / XenonAnalyse / XenonUtils (hedge-dev) | MIT | XEXP patching (`XexPatcher`), independent analysis cross-check | deps: libmspack (LGPL-2.1), tiny-AES-c (Unlicense), TinySHA1 (MIT), tomlplusplus, fmt, xxHash, simde |
| XenosRecomp (hedge-dev) | MIT | shader translation reference | only if the SDK's translator needs cross-checking |
| TTT2 Web (monty488/tekkenwebtournament2) | CC0-1.0 | TTT2 scan tool, switch-table finder, instruction list, runtime notes | public domain; attribute as a courtesy |
| GGPO (pond3r/ggpo) | MIT | rollback netcode | likely with local modifications (kept in-tree with the license) |
| llvm-mingw | Apache-2.0 with LLVM exception; mingw-w64 (various permissive) | compiler bundled in the player package | redistributable |
| libmspack | LGPL-2.1 | LZX (XEX normal compression, disc containers) | dynamic/static linking policy to be decided per LGPL terms |
| Qt 6 (optional) | LGPL-3.0 / commercial | launcher UI candidate | decision in Milestone 8; alternatives: Win32 + ImGui, WebView2 |
| Steamworks SDK (not planned) | proprietary, non-redistributable source | - | TTT2Recomp uses it for transport; this project targets direct P2P + own services to stay free of it |

## Reference-only (no code reuse)

| Project | License | Why reference-only |
|---|---|---|
| Ekspym/tekken6-native-online-source | none published | all rights reserved by default |
| mahmoud-mess/tekken6-recomp | none published | all rights reserved by default |
| shihabal3amri/tekken-tag-tournament-2-pc | none published | all rights reserved by default |
| xenia-project/xenia | BSD-3 | emulator; behaviour reference (its code reaches us through ReXGlue) |

## Policy

* No dependency is added without an entry here, a license check, and a reason.
* Copyleft (LGPL) components are used only as separate shared libraries or in ways their license permits; GPL
  components are not linked into shipped binaries.
* Vendored files carry their license text next to them.
