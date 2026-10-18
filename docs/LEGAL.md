# Content policy and licensing

## What this repository must never contain

* Xbox 360 disc images (`.iso`) or any file extracted from one (`Default.xex`, `DATA*.BIN`, movies, sounds,
  textures, stage data, anything).
* Title update packages (`TU_*`), their contents (`Default.xexp`, `data100.bin`), or save files.
* Decrypted, decompressed or patched executable images, memory dumps, or captures that embed game data.
* **Code generated from the game executable** by any recompiler (`generated/`, `recomp.*.cpp`, `ppc_recomp*`,
  function tables), or large excerpts of disassembly.
* Credentials, tokens, private keys, server certificates, lobby-server addresses meant to stay private.
* Build artifacts and large binaries.

`.gitignore` blocks the usual paths and `scripts/check_repo_hygiene.py` (run by CI and by the optional
pre-commit hook, `scripts/install-git-hooks.sh`) refuses files by extension, name, magic bytes (`XEX2`, STFS
`LIVE`/`PIRS`/`CON`, XDVDFS), size, embedded secrets and generated-code density.

## What is allowed

* Addresses, symbol names, sizes, hashes and other *facts* about the executable needed to configure the
  recompiler or validate a player's copy (e.g. `recomp/ttt2_config.toml`, `known_titles.hpp`). These contain no
  copyrighted expression.
* Screenshots are **not** added for now (the TTT2Recomp project added some at its owner's request; this project
  defers that decision to the owner).

## Players' files

The player must own the game. The launcher reads the player's own disc image and title update, builds the native
executable on the player's machine and keeps everything under the player's install folder. Nothing derived from
the game is uploaded anywhere. Bug reports must contain logs only; the tooling is designed to produce log files
that contain hashes and counts rather than game bytes.

## Licensing of this project's own code

**Decision pending (owner).** Recommended: **MIT**, which is compatible with the BSD-3 (ReXGlue, Xenia), MIT
(GGPO, XenonRecomp, doctest, ImGui) and zlib (SDL3) dependencies and matches the surrounding community. Until a
`LICENSE` file is added, the code is all-rights-reserved by default; contributors should be aware of that.

Third-party code keeps its own license next to it (see `docs/DEPENDENCIES.md`). Code from projects without a
license (Tekken 6 Native Online source, tekken6-recomp, TTT2Recomp) is **not copied**; only publicly stated facts
and design ideas are used, with attribution in `docs/RESEARCH.md`.

## Trademarks

Tekken and Tekken Tag Tournament are trademarks of Bandai Namco Entertainment. Xbox is a trademark of Microsoft.
This project is an independent fan effort, not affiliated with or endorsed by either company.
