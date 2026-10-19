# ADR 0001: Use the ReXGlue SDK for recompilation and runtime

* Status: accepted (2026-10-18), to be re-validated by Milestone 3 measurements
* Deciders: project lead engineer (Claude), owner informed

## Context

Three ways to run TTT2's Xbox 360 code natively were evaluated (see `docs/RESEARCH.md`):

1. **ReXGlue SDK** (BSD-3): static recompiler plus a Xenia-derived runtime. Used by Tekken 6 Native Online, the
   project this one is modelled on, including its rollback netcode.
2. **XenonRecomp** (MIT) plus a hand-written runtime: used by the shipped TTT2Recomp port (runtime unlicensed) and
   by TTT2 Web (runtime CC0 but targeting WebGPU/software rendering).
3. **An emulator (Xenia)**: not viable for rollback (JIT, non-instrumentable, non-deterministic re-simulation).

## Decision

Build on the ReXGlue SDK (pinned release, vendored as a submodule with our own patch set), use GGPO (MIT) for
rollback, keep XenonRecomp/XenonAnalyse/XenonUtils (MIT) as an independent analysis cross-check and as the XEXP
title-update patch tool, and treat TTT2 Web (CC0) as a reusable source of TTT2-specific tooling and runtime
knowledge.

## Consequences

* We inherit a complete runtime (kernel/XAM, memory, VFS, GPU, audio, input, UI) and the exact integration points
  the T6 rollback used (`ReXApp` hooks, mid-asm hooks, `rexcrt`), which removes the largest piece of work.
* We depend on an early-stage SDK with breaking changes: we pin a commit and carry patches.
* ReXGlue codegen cannot apply XEXP patches (verified), so a pre-patch step with XenonUtils' `XexPatcher` is part
  of the pipeline until an own implementation exists.
* If Milestone 3 shows ReXGlue cannot be made to recompile the TTT2 TU3 image (jump tables, instructions), the
  fallback is XenonRecomp codegen (proven on this game) feeding a runtime assembled from ReXGlue's libraries and
  TTT2 Web's CC0 code - the runtime decision survives either way.
