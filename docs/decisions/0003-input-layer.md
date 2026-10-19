# ADR 0003: Keyboard and controller input as a host-side virtual pad with profiles

* Status: accepted (2026-10-19); implementation scheduled for Milestone 5, after the game boots
* Deciders: project lead engineer (Claude); requirements and default layout from the owner

## Context

The owner requires full keyboard play with their Tekken 8 layout (WASD, Numpad 4/5/1/2 for the four limbs,
Numpad 6 and 3 for 1+2 and 3+4, B and V for Start and Back) plus a configurable Tag button (default Numpad 8),
saved profiles, exact simultaneous presses, opposite-direction handling, keyboard in every screen, low latency,
Xbox and PlayStation pads alongside, and inputs that the rollback netcode can carry. The game must keep its
original input behaviour. `docs/INPUT.md` holds the requirements and the design.

The pinned ReXGlue SDK offers three places to do this: (a) its own keyboard driver (`MnkInputDriver`, cvar
bindings, no profiles, no latch, no opposite-direction cleaning on the D-pad); (b) a driver of ours plugged into
its `InputSystem` through the public `InputDriver` interface and `Runtime::Config::input_factory`; (c) host
overrides of `__imp__XamInputGetState` and friends, as the kernel overrides do for XAM stubs.

## Decision

* (b): a project `KeyboardPadDriver` and device assignment inside the SDK's `InputSystem`, created by the host's
  `input_factory` next to the SDK's SDL3 and XInput pad drivers. The game sees one virtual Xbox 360 pad per
  player; pads and keyboard merge through the SDK's existing state merge.
* Bindings are functions in Tekken terms (lp, rp, lk, rk, lp_rp, lk_rk, tag, start, back, four directions), stored
  in TOML profiles under the user data root, translated to pad buttons through a layout table that mirrors the
  game's Controller Settings. The default profile is the owner's layout with Tag on Numpad 8.
* Directions default to the D-pad with opposite inputs resolved to neutral; presses are latched between polls;
  macro keys press both limb buttons; the keyboard feeds player 1 by default and can be moved to player 2.
* Netcode readiness: one per-frame `PlayerInput` word per player is the single point where local devices merge and
  remote or replayed inputs enter, built after cleaning and latching.

## Consequences

* Nothing in the recompiled code or the kernel layer changes for input; the game's buffer, frame delay and
  settings screen are untouched. No Tekken 8 mechanics are introduced.
* The driver depends on the SDK's `InputDriver` interface, which is public and unchanged between v0.10.0 and the
  `development` branch; an interface change is caught by the snapshot evaluation (ADR 0002).
* The pure parts (parser, profiles, policies, latch) are unit-tested without a game; the driver path is tested in
  the headless harness with a license-free program that polls `XamInputGetState`.
* The pad button for Tag and the direction target are assumptions until the owner's first run; both are data in
  the profile, not code.
* (a) is rejected because its bindings are cvars without profiles and it has no latch or cleaning; (c) is rejected
  because it would bypass the SDK's device merge, hot-plug and overlay blocking and tie the host to kernel
  internals.
