# ADR 0002: How the ReXGlue SDK pin moves

* Status: accepted (2026-10-19)
* Deciders: project lead engineer (Claude), owner informed

## Context

`third_party/rexglue-sdk/PIN` names the exact SDK commit every script, workflow and the owner's package build
use (`c94f5eb` = the v0.10.0 release). Upstream keeps a `development` branch ahead of its releases (24 commits
on 2026-10-19, among them atomic `stwcx.`, `sync`/`lwsync`/`eieio` fences, a VMX pack aliasing fix and XAM
content result fixes: all plausible first-boot blockers for a 20-thread game) and warns of breaking changes
between releases. The game has not booted yet, so no failure points at a specific pin, and the owner will run
whatever the pin says.

## Decision

* The pin is a deliberate choice, changed only by a pull request that carries the evaluation below; the
  `development` branch is not tracked blindly.
* Any SDK commit can be evaluated without touching the pin: `REXGLUE_SDK_COMMIT=<sha>
  scripts/fetch-rexglue-sdk.sh` (`-Commit <sha>` on the PowerShell script) and the `sdk_commit` input of
  `sdk-validation.yml` and `rexglue-windows.yml`, which run the whole license-free suite on that snapshot and
  (Windows, `build_host`) produce its package artifact.
* Evaluation checklist for a candidate commit, all license-free:
  1. upstream commit list since the pin, read for codegen, kernel, memory and hook changes;
  2. `scripts/check_instruction_coverage.py --sdk <tree>`: the TTT2 instruction list against the builders;
  3. the SDK's own PPC instruction tests (`ppc_tests`) on the candidate;
  4. `scripts/codegen-smoke-test.sh ... --exec`: codegen plus native runs through the headless harness;
  5. the host (`recomp/`) built unchanged against the candidate's installed package and the normal, stall and
     crash boot tests (`scripts/host-boot-test.sh`, `scripts/run-host.ps1`);
  6. the Windows workflow with `sdk_commit` and `build_host`: tool, smoke test, package, host, boot tests.
* A bump regenerates `src/xex/rexglue_runtime_exports.inc` (`scripts/gen_runtime_exports.py`), re-checks
  `third_party/rexglue-patches/`, updates the version strings in the docs and records the evaluation in
  `docs/RESEARCH.md` 4.1 and `docs/STATUS.md`.
* Once the game boots, a bump needs the owner's boot report on the candidate as well (the real image is the
  only test of the kernel and GPU paths).

## Consequences

* Candidate snapshots cost one Linux and one Windows workflow run (about 15 minutes each) and no local build.
* Host code must stay buildable against the pin and the candidate during an evaluation; where a signature
  moved, the code dispatches on it (`recomp/tests/host/headless_main.cpp`, `BuildLogConfig`).
* The owner's package artifact always matches the pin on `main`; a snapshot's artifact comes from a dispatched
  run and is labelled by its `share/licenses/rexglue-sdk/SOURCE_COMMIT.txt`.
