// Keyboard input core for the native host: binding profiles in Tekken terms, the virtual-pad state the
// game polls, opposite-direction policies, press latching and the per-frame input word the rollback
// netcode will carry (docs/INPUT.md, ADR 0003).
//
// SDK-free on purpose: this file and input_core.cpp depend on the standard library only, so the same
// code is unit-tested in the first-party build (tests/test_input_core.cpp) and compiled into the host,
// where KeyboardPadDriver (Milestone 5, step 2) feeds it the SDK's key events and reads PadState.
// Key names are the SDK's spellings ("W", "Numpad8", "Left", "Shift+Up"), compared case-insensitively.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ttt2::input {

// What a key does, in the game's own terms. The order is the profile's order.
enum class Function : std::uint8_t {
  kUp, kDown, kLeft, kRight,  // directions
  kLP, kRP, kLK, kRK,         // the four limbs: 1, 2, 3, 4
  kLPRP, kLKRK,               // macros: 1+2, 3+4 (press both limb buttons)
  kTag,                       // 5
  kStart, kBack,
  kCount
};
constexpr std::size_t kFunctionCount = static_cast<std::size_t>(Function::kCount);

std::string_view FunctionName(Function f);  // "up", "lp", "lp_rp", "tag", ...
std::optional<Function> ParseFunction(std::string_view name);
bool IsDirection(Function f);
bool IsMacro(Function f);
bool HasLayout(Function f);  // lp, rp, lk, rk, tag, start, back: the functions the [layout] table maps

// The pad control a function is translated to (the game's Controller Settings decide what it means).
enum class PadControl : std::uint8_t { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kStart, kBack, kLS, kRS, kCount };
std::string_view PadControlName(PadControl c);
std::optional<PadControl> ParsePadControl(std::string_view name);

// XInput button bits, the values X_INPUT_GAMEPAD_* / XINPUT_GAMEPAD_* use.
namespace pad {
constexpr std::uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr std::uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr std::uint16_t kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr std::int16_t kStickMax = 32767;
}  // namespace pad

// How two opposite directions held together resolve (docs/INPUT.md 4.3).
enum class SocdPolicy : std::uint8_t { kNeutral, kLastInput, kFirstInput, kUpPriority, kDownPriority };
std::string_view SocdPolicyName(SocdPolicy p);
std::optional<SocdPolicy> ParseSocdPolicy(std::string_view name);

enum class MovementTarget : std::uint8_t { kDpad, kStick, kBoth };
std::string_view MovementTargetName(MovementTarget t);
std::optional<MovementTarget> ParseMovementTarget(std::string_view name);

struct Profile {
  std::string name = "tekken8-numpad";
  // Key names bound to each function; several keys may share a function, a function may be unbound.
  std::array<std::vector<std::string>, kFunctionCount> keys{};
  MovementTarget movement_target = MovementTarget::kDpad;
  SocdPolicy socd_horizontal = SocdPolicy::kNeutral;
  SocdPolicy socd_vertical = SocdPolicy::kNeutral;
  // Pad control per layout function (HasLayout); the rest of the entries are unused.
  std::array<PadControl, kFunctionCount> layout{};
  bool latch_presses = true;
  int player = 1;  // guest user the keyboard feeds, 1-based

  bool operator==(const Profile&) const = default;
};

// The owner's layout: WASD, Numpad 4/5/1/2 for 1/2/3/4, Numpad 6 = 1+2, Numpad 3 = 3+4, Tag = Numpad 8,
// B = Start, V = Back; limbs on X/Y/A/B, Tag on RB (to be confirmed on the real game).
Profile DefaultProfile();

// TOML subset (tables, strings, integers, booleans, comments; one key per line), as docs/INPUT.md shows.
std::string Serialize(const Profile& profile);
struct ParseError {
  int line = 0;  // 1-based; 0 when the error is not tied to a line
  std::string message;
};
// Fills `out` (starting from DefaultProfile() so omitted keys keep their defaults); nullopt on success.
std::optional<ParseError> Parse(std::string_view toml, Profile& out);

// Keys bound to more than one function. Sharing is allowed (the UI shows it), never an error.
struct Conflict {
  std::string key;
  std::vector<Function> functions;
};
std::vector<Conflict> FindConflicts(const Profile& profile);

// What the game's XamInputGetState poll receives for this keyboard.
struct PadState {
  std::uint16_t buttons = 0;
  std::uint8_t left_trigger = 0;
  std::uint8_t right_trigger = 0;
  std::int16_t thumb_lx = 0;
  std::int16_t thumb_ly = 0;  // positive = up, XInput convention
  bool operator==(const PadState&) const = default;
};

// The per-frame word the netcode and replays carry (docs/INPUT.md 4.8): button bits plus the cleaned
// 8-way direction, after opposite-input policies and latching.
struct PlayerInput {
  std::uint16_t buttons = 0;  // pad bits without the D-pad bits
  std::int8_t dx = 0;         // -1 left, +1 right
  std::int8_t dy = 0;         // -1 down, +1 up
  bool operator==(const PlayerInput&) const = default;
};

// Age of key presses at the poll that first reported them (docs/INPUT.md 4.5, --input_stats).
struct LatencyStats {
  std::uint64_t samples = 0;
  std::uint64_t total_us = 0;
  std::uint64_t max_us = 0;
};

class KeyboardPad {
 public:
  explicit KeyboardPad(const Profile& profile);

  // Rebinds to a new profile; every key is released.
  void SetProfile(const Profile& profile);
  const Profile& profile() const { return profile_; }

  // Key events from the window, with the event time in microseconds (any monotonic clock). OS key repeat
  // (a second down without an up) is ignored. Returns false when the key is not bound in the profile.
  bool KeyDown(std::string_view key, std::uint64_t time_us);
  bool KeyUp(std::string_view key, std::uint64_t time_us);
  void ReleaseAll();

  // Builds the pad state the game reads now and consumes the latched presses: a key pressed and released
  // since the previous poll is reported in this one poll (when the profile latches presses).
  PadState Poll(std::uint64_t now_us);
  PlayerInput last_input() const { return last_input_; }

  const LatencyStats& latency() const { return latency_; }
  void ResetLatency() { latency_ = {}; }

 private:
  struct KeyState {
    bool down = false;
    bool latched = false;  // pressed since the last poll
    std::uint64_t sequence = 0;  // press order, for the last/first-input policies
    std::uint64_t pressed_at_us = 0;
    bool age_sampled = false;
  };
  struct Resolved {
    bool left = false, right = false, up = false, down = false;
  };

  bool Active(std::size_t key) const;
  bool FunctionActive(Function f) const;
  // Press order of the active keys of a function: the newest (last input) or the oldest (first input).
  std::uint64_t FunctionSequence(Function f, bool newest) const;
  Resolved ResolveDirections() const;
  void ApplyControl(PadControl control, PadState& state) const;

  Profile profile_;
  std::unordered_map<std::string, std::size_t> key_index_;  // lower-case key name -> keys_ index
  std::vector<KeyState> keys_;
  std::array<std::vector<std::size_t>, kFunctionCount> bound_{};  // key indices per function
  std::uint64_t sequence_ = 0;
  PlayerInput last_input_{};
  LatencyStats latency_{};
};

// Lower-cases ASCII and trims spaces: the key-name normalisation used for matching.
std::string NormalizeKeyName(std::string_view name);

}  // namespace ttt2::input
