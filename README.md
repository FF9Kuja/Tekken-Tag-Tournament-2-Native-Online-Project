# Tekken Tag Tournament 2 Native Online

An independent, in-development **native Windows port of Tekken Tag Tournament 2 (Xbox 360)** with
**rollback netcode**, inspired by [Tekken 6 Native Online](https://github.com/Ekspym/Tekken6-native-online).
The Xbox 360 PowerPC code is statically recompiled to C++ and linked against a native runtime; no emulator runs
during gameplay.

> **Status: early development (Milestones 1-2 of 8 complete, 3 and 4 in progress).** Nothing is playable yet.
> What exists today: the research and architecture, tested tooling to identify, validate and patch a player's own
> game files, a license-free test suite for the recompiler (jump tables, TTT2's instruction set), a switch-table
> finder the recompiler needs, and a headless runtime harness in which recompiled test programs already run
> natively. See [`docs/STATUS.md`](docs/STATUS.md) for an honest, dated account of what works.

**This repository contains no game content.** Players will build the port on their own PC from their own copy of
the game (disc image + Title Update 3). See [`docs/LEGAL.md`](docs/LEGAL.md).

## Documents

| Document | Contents |
|---|---|
| [`docs/RESEARCH.md`](docs/RESEARCH.md) | Milestone 1 research: reference projects, toolchains, TTT2 facts, risks |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Selected architecture and component design |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Milestones 1-8 with exit criteria and owner checkpoints |
| [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) | Every dependency with version and license |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | How to build, test and contribute |
| [`docs/STATUS.md`](docs/STATUS.md) | Session log: completed, verified, blocked |
| [`docs/decisions/`](docs/decisions/) | Architecture decision records |

## Quick start (tooling only)

```sh
# Linux
scripts/setup-ubuntu.sh && scripts/build.sh linux-clang
./out/build/linux-clang/bin/ttt2-xexinfo path/to/Default.xex

# Windows (PowerShell, Visual Studio 2022)
scripts\build.ps1 -Preset windows-msvc
.\out\build\windows-msvc\bin\Release\ttt2-xexinfo.exe path\to\Default.xex
```

`ttt2-xexinfo` reads the plaintext headers of an Xbox 360 executable or title-update patch and reports title ID,
version, media ID, imports, encryption and hashes, so a player's copy can be identified without decrypting it.
`ttt2-iso`, `ttt2-stfs`, `ttt2-xexpatch` and `ttt2-validate` cover the disc image, the title update, patching and
the whole check in one command; `docs/OWNER_CHECKLIST.md` has the copy-paste instructions for a player's own files.

## Planned feature set

Native PC: Windows 10/11 x64, DirectX 12 rendering, audio, Xbox/PlayStation controllers and keyboard, Practice /
Arcade / Versus, 60 fps, modern resolutions. Online: GGPO-style rollback, deterministic simulation, private
matches, lobbies, matchmaking, diagnostics. Launcher: game-file validation, local build from the player's files,
settings, updates. These are goals, not implemented features.

## Credits and references

Tekken 6 Native Online (Ekspym) and tekken6-recomp (mahmoud-mess) for the approach; TTT2Recomp (shihabal3amri)
for proving TTT2 recompiles and runs natively; TTT2 Web (monty488, CC0) for TTT2-specific tooling knowledge;
ReXGlue SDK, XenonRecomp, Xenia and GGPO for the technology. Full list with licenses in
[`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md).

Tekken is a trademark of Bandai Namco Entertainment. This project is not affiliated with or endorsed by Bandai
Namco or Microsoft.
