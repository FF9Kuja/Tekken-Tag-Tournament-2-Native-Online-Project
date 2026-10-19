# third_party/rexglue-sdk

The ReXGlue SDK (BSD-3-Clause) is pinned by `PIN` and fetched on demand with `scripts/fetch-rexglue-sdk.sh`;
the checkout lands in `third_party/rexglue-sdk/src/` (git-ignored). Project-specific SDK changes, when they
become necessary, go to `third_party/rexglue-patches/` as numbered patches applied by the same script.

Build the codegen tool on Linux with `scripts/build-rexglue-linux.sh` (needs Clang 20; package list in
`docs/DEVELOPMENT.md`).
