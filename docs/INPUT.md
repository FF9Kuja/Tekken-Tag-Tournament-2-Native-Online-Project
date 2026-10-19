# Input: keyboard, controllers, profiles

Requirements from the owner (2026-10-19) and the design that meets them. Priority stays with native game
execution (Milestones 3 and 4); the input layer is built in Milestone 5 on top of a game that boots, and its
rollback-facing parts in Milestone 6. Nothing here changes the game's own input behaviour: Tekken Tag Tournament
2 keeps its input buffer, its original frame delay and its controller settings screen; the host only presents a
virtual Xbox 360 pad. No Tekken 8 mechanics (Heat, Special Style) are added.

## 1. Requirements

1. Full keyboard gameplay without a physical controller.
2. Customisable key bindings and saved control profiles.
3. Accurate simultaneous key presses for Tekken commands (1+2, 3+4, f+2, Tag with a button, just-frame inputs).
4. Proper handling of opposite directional inputs (left+right, up+down).
5. Keyboard works in menus, character select, Practice and matches.
6. Low input latency, suitable for a fighting game.
7. Xbox and PlayStation controllers alongside the keyboard.
8. Keyboard inputs integrate with the rollback netcode when it exists.

## 2. Default profile `tekken8-numpad`

The owner's Tekken 8 keyboard layout, plus a Tag button on **Numpad 8**, which no other binding uses and which the
right hand reaches from the punch row (Numpad 4/5). Every entry is rebindable and several keys may share a function.

| Function (Tekken notation) | Key | Virtual pad |
|---|---|---|
| Up / Down / Left / Right | W / S / A / D | D-pad digital directions (stick optional, see 4.3) |
| Left Punch (1) | Numpad 4 | X |
| Right Punch (2) | Numpad 5 | Y |
| Left Kick (3) | Numpad 1 | A |
| Right Kick (4) | Numpad 2 | B |
| LP + RP (1+2) | Numpad 6 | X and Y pressed together |
| LK + RK (3+4) | Numpad 3 | A and B pressed together |
| Tag (5) | Numpad 8 | the game's Tag button (see 4.4) |
| Start / Menu | B | Start |
| Back / View | V | Back |

The virtual pad column follows the game's default controller layout as this project assumes it (X/Y/A/B for the
four limbs, the console-era convention). It is confirmed, or corrected in one table, on the owner's first run from
the game's own Controller Settings screen. The macro keys deliberately press the two limb buttons rather than a
dedicated 1+2 / 3+4 pad button, so they work whatever the in-game layout says.

## 3. What the ReXGlue SDK already provides (measured on the pinned v0.10.0)

* An `InputSystem` with pluggable drivers (`rex::input::InputDriver`: enumerate devices, `GetDeviceState`,
  capabilities, vibration, keystrokes). `XamInputGetState` for guest user N merges the states of every device
  assigned to N: buttons OR, triggers max, stick axes larger magnitude (`state_merge.h`). Assignments: `SlotAssignment`
  (pad ordinal N feeds user N, synthetic devices such as a keyboard feed user 0) or `SharedAssignment` (everything
  feeds user 0). Devices are re-enumerated on every poll, so hot-plug works.
* Controllers: an SDL3 gamepad driver (Xbox, DualShock and DualSense through SDL's mapping database) and an XInput
  driver on Windows (`input_backend` cvar). The `development` branch adds deadzones, vibration on/off, hot-plug
  notices and the real XInput subtype.
* A keyboard/mouse driver (`MnkInputDriver`, off by default: `mnk_mode`): one synthetic device "Keyboard and Mouse";
  bindings are string cvars (`keybind_a = "Semicolon,Space"`, comma-separated alternatives, exact `Shift+`/`Ctrl+`/
  `Alt+` modifiers); the key-down table is sampled when the guest polls; WASD drive the left stick (opposite keys
  cancel to neutral because the axis sums), Shift+arrows drive the D-pad bits with no opposite-direction cleaning;
  key events are also queued as pad keystrokes for `XamInputGetKeystroke`. The `development` branch adds a raw
  keyboard passthrough device and blocks guest input while an overlay is open.
* The app supplies the `InputSystem` through `Runtime::Config::input_factory` (`ReXApp` installs the default
  factory), so the host can build its own system: the SDK's pad drivers plus a driver of ours, with our own
  assignment.

What is missing for the requirements: a profile model (named, saved, switchable), per-frame press latching,
opposite-direction policies on digital directions, macro keys, a direction target choice, player-slot choice for
the keyboard, and an in-game rebinding screen.

## 4. Design

### 4.1 Where it lives

`recomp/src/input/`, compiled into the host. `Ttt2App::OnPreSetup` replaces `input_factory` with one that creates
the SDK's SDL3 (and XInput on Windows) drivers, the project's `KeyboardPadDriver` and the project's device
assignment. The game keeps calling `XamInputGetState` / `XamInputGetKeystroke`; nothing in the recompiled code or
the kernel changes. The SDK's own `MnkInputDriver` stays off.

### 4.2 Profiles

* One TOML file per profile under the user data root (`<user data root>/input/profiles/<name>.toml`; the root
  the host already logs as "user data root"), plus `input/input.toml` holding the active profile name and the
  player slot. The default profile of section 2 is embedded in the host and written out on first run, so it can be
  edited by hand or in game. Profiles are plain text with key names only: shareable, diffable, no game data.
* Schema (one key or a comma-separated list of alternatives per function, SDK key names such as `Numpad8`,
  `W`, `Shift+Up`):

  ```toml
  [profile]
  name = "tekken8-numpad"

  [movement]
  up = "W"
  down = "S"
  left = "A"
  right = "D"
  target = "dpad"             # "dpad" (default) | "stick" | "both"
  socd_horizontal = "neutral" # "neutral" (default) | "last_input" | "first_input"
  socd_vertical = "neutral"   # "neutral" (default) | "last_input" | "first_input" | "up" | "down"

  [buttons]
  lp = "Numpad4"
  rp = "Numpad5"
  lk = "Numpad1"
  rk = "Numpad2"
  lp_rp = "Numpad6"
  lk_rk = "Numpad3"
  tag = "Numpad8"
  start = "B"
  back = "V"

  [layout]                    # function -> pad control, mirrors the game's Controller Settings
  lp = "X"
  rp = "Y"
  lk = "A"
  rk = "B"
  tag = "RB"                  # to be confirmed on the real game
  start = "Start"
  back = "Back"

  [options]
  latch_presses = true        # a key pressed and released between two polls still counts for one poll
  player = 1                  # guest user the keyboard feeds (1 or 2)
  ```

  Key names are the SDK's (`W`, `Numpad8`, `Left`, `Shift+Up`, ...); a value may list alternatives separated by
  commas. The files use a TOML subset (tables, strings, integers, booleans, comments), one key per line.

* Switching profiles, renaming, duplicating and resetting to the default happen in the overlay (4.6) or by editing
  the files; `--input_profile <name>` selects one for a run.

### 4.3 Directions

* Default target is the D-pad: that is the digital 8-way input the console pad gives the game. `stick` and
  `both` exist for screens that turn out to read only the stick (verified on the real game, section 6).
* Opposite inputs are resolved before the pad state is built, per axis, by policy: `neutral` (default on both
  axes: what a physical D-pad produces, and the tournament-standard cleaning), `last_input` (the most recently
  pressed direction wins and releasing it returns to the one still held), `first_input`, and `up`/`down` priority
  for the vertical axis. The game never sees left+right or up+down at once.
* Diagonals are simply two cleaned directions held together (d/f, u/b and so on), as on a pad.

### 4.4 Buttons, macros and Tag

* Each function maps to one pad button through the `[layout]` table; limb functions default to X/Y/A/B. The
  macro functions `lp_rp` and `lk_rk` set both limb bits in the same poll, which is what the game expects from two
  buttons pressed together on a pad.
* Tag is a function of its own, bound to Numpad 8 by default and translated through `[layout] tag`; the assumed
  pad button is the right bumper until the game's Controller Settings screen confirms it. Combinations with Tag
  (Tag with a direction or a button for Tag Assault and Tag Crash) are just simultaneous presses and need no
  special handling.
* Start and Back map to the pad's Start and Back, so the same keys work in every menu, in character select and
  in Practice: those screens read the same pad as the match does.

### 4.5 Timing: simultaneity and latency

* Key events arrive on the UI thread (`WindowInputListener::OnKeyDown/OnKeyUp` from SDL, with SDL's timestamps);
  OS key repeat is ignored (down/up transitions only). The driver keeps a key-down table and, with
  `latch_presses`, a "pressed since the last poll" table: a key tapped and released between two guest polls is
  reported pressed for exactly one poll instead of being lost, and two keys pressed in the same frame are reported
  together. Polls read the tables under a mutex, build directions (4.3) and buttons (4.4) and clear the latch;
  nothing is buffered across frames, so the only latency between key and game is the time to the game's next
  poll (the game polls once per frame).
* Debug statistics: the driver records the age of each key event at the poll that consumed it and logs the
  median and worst case per minute when `--input_stats` is on, so latency is measured on the owner's machine
  rather than assumed. Display latency (present mode, vsync) is graphics work and is listed under Milestone 5.

### 4.6 Rebinding in game

The SDK's ImGui overlay hosts a "Controls" dialog (`OnCreateDialogs`): profile list, press-to-bind for each
function (the next key down is taken; Escape cancels; conflicts within a profile are shown), direction target and
opposite-input policies, player slot, save/duplicate/reset. The guest reads a neutral pad while the dialog is open
(the `development` branch's UI blocker, or the same effect in our driver on the pin).

### 4.7 Controllers alongside the keyboard

* Pads come from the SDK's SDL3 driver (Xbox and PlayStation layouts through SDL's gamepad mappings, including
  DualShock 4 and DualSense) and from XInput on Windows; both merge with the keyboard for the same guest user
  (buttons OR), so a player may mix a pad and keys.
* Local Versus: the project's assignment puts pad 1 and the keyboard on player 1 and pad 2 on player 2 by
  default; `player = 2` in the profile moves the keyboard to player 2 (keyboard against a pad).
* Pad bindings follow the game's own Controller Settings; the host does not remap pads.

### 4.8 Rollback netcode (Milestone 6)

The game polls one virtual pad per player. The netcode needs a per-frame input word per player, so the host adds
one choke point per guest user: a `PlayerInput` (16-bit button mask plus the 8-way direction, 3 bytes) built once
per frame from the merged, cleaned local devices. Locally it is what the game's poll returns; in a match it is
what GGPO transports, predicts and rolls back; in a replay it is what is read from the recording. Keyboard
cleaning (4.3) and latching (4.5) therefore happen before the word is formed, so both sides of a match see the
same input and no raw key timing leaks into determinism. The input hook described in `docs/ARCHITECTURE.md`
(where the game builds both players' input entries) consumes these words.

## 5. Implementation order (after the game boots)

1. `recomp/src/input/`: binding parser, profile load/save (default embedded), opposite-input policies, latch,
   macro expansion: pure code with doctest unit tests (no game, no window). **Done** (`input_core.{h,cpp}`,
   `tests/test_input_core.cpp`): `Profile` / `DefaultProfile()` / `Serialize` / `Parse` / `FindConflicts` and
   `KeyboardPad` (`KeyDown` / `KeyUp` / `Poll` -> `PadState`, `last_input()` -> `PlayerInput`, latency
   statistics). The SDK-free core is compiled into the host but not yet driven by it.
2. `KeyboardPadDriver` and the device assignment on the SDK's `InputDriver` interface; the host's
   `input_factory`; a license-free guest program that polls `XamInputGetState` and stores the button word, run in
   the headless harness with injected key events, so the whole path is tested in CI before the game.
3. Overlay dialog; `--input_profile`; input statistics.
4. Owner verification on the real game (section 6), then the layout table and any screen-specific fixes.
5. Milestone 6: `PlayerInput` words, recording/replay, GGPO injection.

## 6. Verification on the real game (owner)

A short checklist run from the keyboard only, no pad connected: title screen and main menu navigation with WASD
and B/V; character select for both players; Practice: 1, 2, 3, 4 singly; 1+2 and 3+4 via Numpad 6 and 3 and via the
two keys together; Tag via Numpad 8, Tag Assault with Tag plus a button; d/f, u/b and a back-dash; a just-frame
input (EWGF: f, n, d, d/f+2) repeated ten times; left+right held together (expect neutral); a full match. Then with
a pad connected: pad and keyboard together on player 1, keyboard as player 2. The results decide the default
direction target and confirm the layout table.
