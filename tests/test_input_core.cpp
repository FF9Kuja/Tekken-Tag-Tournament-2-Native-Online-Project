// Unit tests of the keyboard input core (recomp/src/input/input_core.*): the owner's default profile,
// profile files, opposite-direction policies, press latching, macros and the netcode input word.
#include <doctest.h>

#include <string>

#include "input/input_core.h"

using namespace ttt2::input;

namespace {

std::size_t idx(Function f) { return static_cast<std::size_t>(f); }

}  // namespace

TEST_CASE("default profile is the owner's Tekken 8 layout with Tag on Numpad 8") {
  const Profile p = DefaultProfile();
  CHECK(p.name == "tekken8-numpad");
  CHECK(p.keys[idx(Function::kUp)] == std::vector<std::string>{"W"});
  CHECK(p.keys[idx(Function::kDown)] == std::vector<std::string>{"S"});
  CHECK(p.keys[idx(Function::kLeft)] == std::vector<std::string>{"A"});
  CHECK(p.keys[idx(Function::kRight)] == std::vector<std::string>{"D"});
  CHECK(p.keys[idx(Function::kLP)] == std::vector<std::string>{"Numpad4"});
  CHECK(p.keys[idx(Function::kRP)] == std::vector<std::string>{"Numpad5"});
  CHECK(p.keys[idx(Function::kLK)] == std::vector<std::string>{"Numpad1"});
  CHECK(p.keys[idx(Function::kRK)] == std::vector<std::string>{"Numpad2"});
  CHECK(p.keys[idx(Function::kLPRP)] == std::vector<std::string>{"Numpad6"});
  CHECK(p.keys[idx(Function::kLKRK)] == std::vector<std::string>{"Numpad3"});
  CHECK(p.keys[idx(Function::kTag)] == std::vector<std::string>{"Numpad8"});
  CHECK(p.keys[idx(Function::kStart)] == std::vector<std::string>{"B"});
  CHECK(p.keys[idx(Function::kBack)] == std::vector<std::string>{"V"});
  CHECK(p.layout[idx(Function::kLP)] == PadControl::kX);
  CHECK(p.layout[idx(Function::kRP)] == PadControl::kY);
  CHECK(p.layout[idx(Function::kLK)] == PadControl::kA);
  CHECK(p.layout[idx(Function::kRK)] == PadControl::kB);
  CHECK(p.layout[idx(Function::kTag)] == PadControl::kRB);
  CHECK(p.layout[idx(Function::kStart)] == PadControl::kStart);
  CHECK(p.layout[idx(Function::kBack)] == PadControl::kBack);
  CHECK(p.movement_target == MovementTarget::kDpad);
  CHECK(p.socd_horizontal == SocdPolicy::kNeutral);
  CHECK(p.socd_vertical == SocdPolicy::kNeutral);
  CHECK(p.latch_presses);
  CHECK(p.player == 1);
  CHECK(FindConflicts(p).empty());
}

TEST_CASE("profile files round-trip through the TOML subset") {
  const Profile original = DefaultProfile();
  const std::string text = Serialize(original);
  CHECK(text.find("tag = \"Numpad8\"") != std::string::npos);
  CHECK(text.find("[layout]") != std::string::npos);
  Profile parsed;
  auto err = Parse(text, parsed);
  if (err) FAIL(err->message);
  CHECK(parsed == original);

  SUBCASE("every field survives a change") {
    Profile changed = original;
    changed.name = "custom \"one\"";
    changed.keys[idx(Function::kTag)] = {"Space", "Numpad0"};
    changed.keys[idx(Function::kBack)] = {};
    changed.movement_target = MovementTarget::kBoth;
    changed.socd_horizontal = SocdPolicy::kLastInput;
    changed.socd_vertical = SocdPolicy::kUpPriority;
    changed.layout[idx(Function::kTag)] = PadControl::kLB;
    changed.latch_presses = false;
    changed.player = 2;
    Profile again;
    err = Parse(Serialize(changed), again);
    if (err) FAIL(err->message);
    CHECK(again == changed);
  }
}

TEST_CASE("parsing accepts comments, spacing and case, and reports errors with their line") {
  Profile p;
  const char* text =
      "# a comment\n"
      "[Buttons]\n"
      "  LP = \"numpad4\"   # trailing comment\n"
      "tag = \"Space , Numpad0\"\r\n"
      "[movement]\n"
      "socd_vertical = \"up\"\n"
      "[options]\n"
      "player = 2\n";
  auto err = Parse(text, p);
  if (err) FAIL(err->message);
  CHECK(p.keys[idx(Function::kLP)] == std::vector<std::string>{"numpad4"});
  CHECK(p.keys[idx(Function::kTag)] == std::vector<std::string>{"Space", "Numpad0"});
  CHECK(p.socd_vertical == SocdPolicy::kUpPriority);
  CHECK(p.player == 2);
  CHECK(p.keys[idx(Function::kUp)] == std::vector<std::string>{"W"});  // defaults kept

  auto expect_error = [](const char* toml, int line) {
    Profile q;
    auto e = Parse(toml, q);
    REQUIRE(e);
    CHECK(e->line == line);
    CHECK(!e->message.empty());
  };
  expect_error("[buttons]\nheat = \"H\"\n", 2);                      // Tekken 8 function: unknown
  expect_error("[movement]\nsocd_horizontal = \"up\"\n", 2);         // up priority is vertical only
  expect_error("[movement]\ntarget = \"mouse\"\n", 2);
  expect_error("[layout]\nlp = \"Z\"\n", 2);
  expect_error("[options]\nplayer = 7\n", 2);
  expect_error("[options]\nlatch_presses = yes\n", 2);
  expect_error("[buttons]\nlp = \"Numpad4\n", 2);                   // unterminated string
  expect_error("lp = \"Numpad4\"\n", 1);                             // key outside a table
  expect_error("[weapons]\n", 1);
  expect_error("[buttons]\nup = \"W\"\n", 2);                        // direction in the wrong table
}

TEST_CASE("conflicts list keys shared by several functions") {
  Profile p = DefaultProfile();
  p.keys[idx(Function::kTag)] = {"numpad4"};  // same key as LP, different case
  auto conflicts = FindConflicts(p);
  REQUIRE(conflicts.size() == 1);
  CHECK(NormalizeKeyName(conflicts[0].key) == "numpad4");
  CHECK(conflicts[0].functions == std::vector<Function>{Function::kLP, Function::kTag});
}

TEST_CASE("buttons, macros and Tag reach the pad bits of the layout") {
  KeyboardPad pad(DefaultProfile());
  CHECK(pad.Poll(0) == PadState{});

  CHECK(pad.KeyDown("Numpad4", 10));
  CHECK(pad.Poll(20).buttons == pad::kX);
  pad.KeyUp("Numpad4", 30);
  CHECK(pad.Poll(40) == PadState{});

  pad.KeyDown("Numpad6", 50);  // 1+2 macro: both limb buttons in the same poll
  CHECK(pad.Poll(60).buttons == (pad::kX | pad::kY));
  pad.KeyUp("Numpad6", 70);

  pad.KeyDown("Numpad1", 80);
  pad.KeyDown("Numpad2", 81);  // two keys pressed in one frame
  CHECK(pad.Poll(90).buttons == (pad::kA | pad::kB));
  pad.KeyUp("Numpad1", 91);
  pad.KeyUp("Numpad2", 91);

  pad.KeyDown("Numpad3", 100);
  pad.KeyDown("Numpad8", 101);  // 3+4 with Tag
  CHECK(pad.Poll(110).buttons == (pad::kA | pad::kB | pad::kRB));
  pad.ReleaseAll();

  pad.KeyDown("b", 120);
  pad.KeyDown("v", 121);
  CHECK(pad.Poll(130).buttons == (pad::kStart | pad::kBack));
  pad.ReleaseAll();
  CHECK(pad.Poll(140) == PadState{});

  CHECK_FALSE(pad.KeyDown("F12", 150));  // not bound
}

TEST_CASE("directions: D-pad by default, diagonals, neutral on opposite inputs") {
  KeyboardPad pad(DefaultProfile());
  pad.KeyDown("A", 0);
  PadState s = pad.Poll(1);
  CHECK(s.buttons == pad::kDpadLeft);
  CHECK(s.thumb_lx == 0);
  CHECK(pad.last_input() == PlayerInput{0, -1, 0});

  pad.KeyDown("W", 2);  // u/b
  s = pad.Poll(3);
  CHECK(s.buttons == (pad::kDpadLeft | pad::kDpadUp));
  CHECK(pad.last_input() == PlayerInput{0, -1, 1});

  pad.KeyDown("D", 4);  // left + right held: neutral horizontally, up stays
  s = pad.Poll(5);
  CHECK(s.buttons == pad::kDpadUp);
  CHECK(pad.last_input() == PlayerInput{0, 0, 1});

  pad.KeyDown("S", 6);  // up + down too: fully neutral
  CHECK(pad.Poll(7) == PadState{});
  pad.KeyUp("A", 8);
  pad.KeyUp("W", 9);
  s = pad.Poll(10);  // d/f remains
  CHECK(s.buttons == (pad::kDpadRight | pad::kDpadDown));
  CHECK(pad.last_input() == PlayerInput{0, 1, -1});
}

TEST_CASE("opposite-direction policies: last input, first input, up and down priority") {
  Profile p = DefaultProfile();
  p.socd_horizontal = SocdPolicy::kLastInput;
  p.socd_vertical = SocdPolicy::kLastInput;
  KeyboardPad pad(p);
  pad.KeyDown("A", 0);
  pad.KeyDown("D", 1);
  CHECK(pad.Poll(2).buttons == pad::kDpadRight);  // the newer press wins
  pad.KeyUp("D", 3);
  CHECK(pad.Poll(4).buttons == pad::kDpadLeft);   // releasing it returns to the held one
  pad.KeyDown("D", 5);
  pad.KeyDown("A", 6);  // A again while D is held: A is the newer one now? no: A is still held from 0
  CHECK(pad.Poll(7).buttons == pad::kDpadRight);  // repeat of a held key does not count as a new press
  pad.ReleaseAll();

  p.socd_horizontal = SocdPolicy::kFirstInput;
  pad.SetProfile(p);
  pad.KeyDown("A", 10);
  pad.KeyDown("D", 11);
  CHECK(pad.Poll(12).buttons == pad::kDpadLeft);
  pad.ReleaseAll();

  p.socd_vertical = SocdPolicy::kUpPriority;
  pad.SetProfile(p);
  pad.KeyDown("S", 20);
  pad.KeyDown("W", 21);
  CHECK(pad.Poll(22).buttons == pad::kDpadUp);
  pad.ReleaseAll();

  p.socd_vertical = SocdPolicy::kDownPriority;
  pad.SetProfile(p);
  pad.KeyDown("W", 30);
  pad.KeyDown("S", 31);
  CHECK(pad.Poll(32).buttons == pad::kDpadDown);
}

TEST_CASE("movement target: stick and both") {
  Profile p = DefaultProfile();
  p.movement_target = MovementTarget::kStick;
  KeyboardPad pad(p);
  pad.KeyDown("D", 0);
  pad.KeyDown("S", 0);
  PadState s = pad.Poll(1);
  CHECK(s.buttons == 0);
  CHECK(s.thumb_lx == pad::kStickMax);
  CHECK(s.thumb_ly == -pad::kStickMax);
  CHECK(pad.last_input() == PlayerInput{0, 1, -1});

  p.movement_target = MovementTarget::kBoth;
  pad.SetProfile(p);
  pad.KeyDown("A", 2);
  pad.KeyDown("W", 2);
  s = pad.Poll(3);
  CHECK(s.buttons == (pad::kDpadLeft | pad::kDpadUp));
  CHECK(s.thumb_lx == -pad::kStickMax);
  CHECK(s.thumb_ly == pad::kStickMax);
}

TEST_CASE("a key tapped between two polls is reported for exactly one poll") {
  KeyboardPad pad(DefaultProfile());
  pad.KeyDown("Numpad5", 100);
  pad.KeyUp("Numpad5", 104);  // 4 microseconds, well inside one frame
  CHECK(pad.Poll(1000).buttons == pad::kY);
  CHECK(pad.Poll(2000) == PadState{});

  // A tapped direction takes part in the opposite-input resolution of that poll.
  Profile p = DefaultProfile();
  p.socd_horizontal = SocdPolicy::kLastInput;
  pad.SetProfile(p);
  pad.KeyDown("A", 10);
  pad.KeyDown("D", 11);
  pad.KeyUp("D", 12);
  CHECK(pad.Poll(20).buttons == pad::kDpadRight);  // the tap was the last input
  CHECK(pad.Poll(30).buttons == pad::kDpadLeft);   // and is gone afterwards

  SUBCASE("without latching the tap is lost") {
    Profile q = DefaultProfile();
    q.latch_presses = false;
    KeyboardPad plain(q);
    plain.KeyDown("Numpad5", 100);
    plain.KeyUp("Numpad5", 104);
    CHECK(plain.Poll(1000) == PadState{});
    plain.KeyDown("Numpad5", 2000);
    CHECK(plain.Poll(2001).buttons == pad::kY);
  }
}

TEST_CASE("key repeat and release-all") {
  KeyboardPad pad(DefaultProfile());
  pad.KeyDown("Numpad4", 0);
  pad.KeyDown("Numpad4", 500);  // OS auto-repeat
  pad.KeyUp("Numpad4", 600);
  pad.Poll(650);                // consumes the latched press
  CHECK(pad.Poll(700) == PadState{});
  pad.KeyDown("W", 800);
  pad.KeyDown("Numpad8", 800);
  pad.ReleaseAll();             // focus lost
  CHECK(pad.Poll(900) == PadState{});
}

TEST_CASE("latency statistics measure the age of a press at the poll that first reports it") {
  KeyboardPad pad(DefaultProfile());
  pad.KeyDown("Numpad4", 1000);
  pad.Poll(5000);
  pad.Poll(9000);  // still held: not sampled again
  pad.KeyDown("Numpad5", 10000);
  pad.Poll(10500);
  const LatencyStats& st = pad.latency();
  CHECK(st.samples == 2);
  CHECK(st.total_us == 4500);
  CHECK(st.max_us == 4000);
  pad.ResetLatency();
  CHECK(pad.latency().samples == 0);
}

TEST_CASE("names parse both ways") {
  for (std::size_t i = 0; i < kFunctionCount; ++i) {
    const Function f = static_cast<Function>(i);
    CHECK(ParseFunction(FunctionName(f)) == f);
  }
  CHECK(ParseFunction("LP_RP") == Function::kLPRP);
  CHECK_FALSE(ParseFunction("heat"));
  CHECK(ParsePadControl("rb") == PadControl::kRB);
  CHECK(ParseSocdPolicy("Last_Input") == SocdPolicy::kLastInput);
  CHECK(ParseMovementTarget("Both") == MovementTarget::kBoth);
  CHECK(NormalizeKeyName("  Numpad8 ") == "numpad8");
  CHECK(IsMacro(Function::kLKRK));
  CHECK_FALSE(HasLayout(Function::kLPRP));
  CHECK(HasLayout(Function::kTag));
}
