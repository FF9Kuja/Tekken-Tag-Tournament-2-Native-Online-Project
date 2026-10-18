# recomp/ - the ReXGlue recompilation project (Milestone 3)

This directory will hold the ReXGlue project files for Tekken Tag Tournament 2:

* `ttt2_manifest.toml` - points `rexglue codegen` at the player's (local, never committed) patched `Default.xex`.
* `ttt2_config.toml` - addresses only: function boundaries, switch tables, invalid-instruction regions, mid-asm
  hooks, `[rexcrt]` redirects. Safe to commit (facts, no game bytes).
* `tools/` - post-codegen patch tools (netplay instrumentation) and the switch-table finder.
* `generated/` - codegen output. **Ignored by git and refused by the hygiene check.**

Nothing here is populated yet; the files are created from `rexglue init` once the SDK is vendored and the Linux
build of the `rexglue` tool is scripted (see `docs/ROADMAP.md`, Milestone 3).
