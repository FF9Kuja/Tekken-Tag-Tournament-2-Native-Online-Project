# third_party/rexglue-sdk

The ReXGlue SDK (BSD-3-Clause) is pinned by `PIN` and fetched on demand with `scripts/fetch-rexglue-sdk.sh`;
the checkout lands in `third_party/rexglue-sdk/src/` (git-ignored). Project-specific SDK changes go to
`third_party/rexglue-patches/` as numbered patches applied by the same script (and `fetch-rexglue-sdk.ps1`):

* `0001-fall-through.patch` - codegen: code that runs on past a function's recorded end continues into the code
  that follows (a goto, or a tail call of the function there) instead of returning silently; every site is logged
  and marked, and `rexglue codegen` logs `TTT2 SDK patches: 0001-fall-through`, which the owner pipeline checks
  (`docs/RESEARCH.md` 8g).

An SDK package built before a patch was added lacks it: download the newest `rexglue-sdk-install-win-amd64`
artifact of the Windows workflow on `main`, or rebuild with `scripts/build-rexglue-*`.

Build the codegen tool on Linux with `scripts/build-rexglue-linux.sh` (needs Clang 20; package list in
`docs/DEVELOPMENT.md`).
