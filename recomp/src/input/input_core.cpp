// Keyboard input core: see input_core.h and docs/INPUT.md.
#include "input_core.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

namespace ttt2::input {

namespace {

constexpr std::array<std::string_view, kFunctionCount> kFunctionNames = {
    "up", "down", "left", "right", "lp", "rp", "lk", "rk", "lp_rp", "lk_rk", "tag", "start", "back"};
constexpr std::array<std::string_view, static_cast<std::size_t>(PadControl::kCount)> kPadControlNames = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "LS", "RS"};
constexpr std::array<std::string_view, 5> kSocdNames = {"neutral", "last_input", "first_input", "up", "down"};
constexpr std::array<std::string_view, 3> kTargetNames = {"dpad", "stick", "both"};

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::vector<std::string> SplitKeys(std::string_view value) {
  std::vector<std::string> keys;
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    std::string_view token = Trim(value.substr(0, comma));
    if (!token.empty()) keys.emplace_back(token);
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return keys;
}

std::string JoinKeys(const std::vector<std::string>& keys) {
  std::string out;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i) out += ", ";
    out += keys[i];
  }
  return out;
}

std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

// Parses a TOML string literal "..." with the escapes \" \\ \n \t; returns false on a malformed value.
bool ParseQuoted(std::string_view value, std::string& out) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
  out.clear();
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    char c = value[i];
    if (c == '\\') {
      if (i + 2 >= value.size()) return false;
      char e = value[++i];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        default: return false;
      }
    } else if (c == '"') {
      return false;  // an unescaped quote inside the literal
    } else {
      out += c;
    }
  }
  return true;
}

// Strips a trailing comment, honouring quotes.
std::string_view StripComment(std::string_view line) {
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '\\' && quoted) {
      ++i;
    } else if (c == '"') {
      quoted = !quoted;
    } else if (c == '#' && !quoted) {
      return line.substr(0, i);
    }
  }
  return line;
}

const char* kLayoutDefaults[] = {"X", "Y", "A", "B", "RB", "Start", "Back"};  // lp rp lk rk tag start back

}  // namespace

std::string_view FunctionName(Function f) { return kFunctionNames[static_cast<std::size_t>(f)]; }

std::optional<Function> ParseFunction(std::string_view name) {
  for (std::size_t i = 0; i < kFunctionCount; ++i) {
    if (EqualsIgnoreCase(kFunctionNames[i], name)) return static_cast<Function>(i);
  }
  return std::nullopt;
}

bool IsDirection(Function f) { return f <= Function::kRight; }
bool IsMacro(Function f) { return f == Function::kLPRP || f == Function::kLKRK; }
bool HasLayout(Function f) { return !IsDirection(f) && !IsMacro(f) && f != Function::kCount; }

std::string_view PadControlName(PadControl c) { return kPadControlNames[static_cast<std::size_t>(c)]; }

std::optional<PadControl> ParsePadControl(std::string_view name) {
  for (std::size_t i = 0; i < kPadControlNames.size(); ++i) {
    if (EqualsIgnoreCase(kPadControlNames[i], name)) return static_cast<PadControl>(i);
  }
  return std::nullopt;
}

std::string_view SocdPolicyName(SocdPolicy p) { return kSocdNames[static_cast<std::size_t>(p)]; }

std::optional<SocdPolicy> ParseSocdPolicy(std::string_view name) {
  for (std::size_t i = 0; i < kSocdNames.size(); ++i) {
    if (EqualsIgnoreCase(kSocdNames[i], name)) return static_cast<SocdPolicy>(i);
  }
  return std::nullopt;
}

std::string_view MovementTargetName(MovementTarget t) { return kTargetNames[static_cast<std::size_t>(t)]; }

std::optional<MovementTarget> ParseMovementTarget(std::string_view name) {
  for (std::size_t i = 0; i < kTargetNames.size(); ++i) {
    if (EqualsIgnoreCase(kTargetNames[i], name)) return static_cast<MovementTarget>(i);
  }
  return std::nullopt;
}

std::string NormalizeKeyName(std::string_view name) { return Lower(Trim(name)); }

Profile DefaultProfile() {
  Profile p;
  p.name = "tekken8-numpad";
  auto bind = [&p](Function f, const char* key) { p.keys[static_cast<std::size_t>(f)] = {key}; };
  bind(Function::kUp, "W");
  bind(Function::kDown, "S");
  bind(Function::kLeft, "A");
  bind(Function::kRight, "D");
  bind(Function::kLP, "Numpad4");
  bind(Function::kRP, "Numpad5");
  bind(Function::kLK, "Numpad1");
  bind(Function::kRK, "Numpad2");
  bind(Function::kLPRP, "Numpad6");
  bind(Function::kLKRK, "Numpad3");
  bind(Function::kTag, "Numpad8");
  bind(Function::kStart, "B");
  bind(Function::kBack, "V");
  std::size_t i = 0;
  for (Function f : {Function::kLP, Function::kRP, Function::kLK, Function::kRK, Function::kTag, Function::kStart,
                     Function::kBack}) {
    p.layout[static_cast<std::size_t>(f)] = *ParsePadControl(kLayoutDefaults[i++]);
  }
  return p;
}

std::string Serialize(const Profile& profile) {
  std::string out;
  out += "# Tekken Tag Tournament 2 Native Online: keyboard profile (docs/INPUT.md). Key names are the\n";
  out += "# host's (W, Numpad8, Left, Shift+Up, ...); a value may list alternatives separated by commas.\n";
  out += "[profile]\nname = " + Quote(profile.name) + "\n\n[movement]\n";
  for (Function f : {Function::kUp, Function::kDown, Function::kLeft, Function::kRight}) {
    out += std::string(FunctionName(f)) + " = " + Quote(JoinKeys(profile.keys[static_cast<std::size_t>(f)])) + "\n";
  }
  out += "target = " + Quote(MovementTargetName(profile.movement_target)) + "\n";
  out += "socd_horizontal = " + Quote(SocdPolicyName(profile.socd_horizontal)) + "\n";
  out += "socd_vertical = " + Quote(SocdPolicyName(profile.socd_vertical)) + "\n\n[buttons]\n";
  for (std::size_t i = static_cast<std::size_t>(Function::kLP); i < kFunctionCount; ++i) {
    out += std::string(kFunctionNames[i]) + " = " + Quote(JoinKeys(profile.keys[i])) + "\n";
  }
  out += "\n[layout]\n";
  for (std::size_t i = 0; i < kFunctionCount; ++i) {
    const Function f = static_cast<Function>(i);
    if (!HasLayout(f)) continue;
    out += std::string(kFunctionNames[i]) + " = " + Quote(PadControlName(profile.layout[i])) + "\n";
  }
  out += "\n[options]\n";
  out += std::string("latch_presses = ") + (profile.latch_presses ? "true" : "false") + "\n";
  out += "player = " + std::to_string(profile.player) + "\n";
  return out;
}

std::optional<ParseError> Parse(std::string_view toml, Profile& out) {
  Profile p = DefaultProfile();
  std::string table;
  int line_no = 0;
  std::size_t pos = 0;
  while (pos <= toml.size()) {
    const std::size_t nl = toml.find('\n', pos);
    std::string_view raw = toml.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = (nl == std::string_view::npos) ? toml.size() + 1 : nl + 1;
    ++line_no;
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    std::string_view line = Trim(StripComment(raw));
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') return ParseError{line_no, "malformed table header"};
      table = Lower(Trim(line.substr(1, line.size() - 2)));
      if (table != "profile" && table != "movement" && table != "buttons" && table != "layout" && table != "options") {
        return ParseError{line_no, "unknown table [" + table + "]"};
      }
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) return ParseError{line_no, "expected key = value"};
    const std::string key = Lower(Trim(line.substr(0, eq)));
    const std::string_view value = Trim(line.substr(eq + 1));
    if (key.empty() || value.empty()) return ParseError{line_no, "expected key = value"};
    if (table.empty()) return ParseError{line_no, "key outside of a table"};

    std::string str;
    const bool is_string = ParseQuoted(value, str);
    if (value.front() == '"' && !is_string) return ParseError{line_no, "malformed string"};

    if (table == "profile") {
      if (key != "name") return ParseError{line_no, "unknown key '" + key + "' in [profile]"};
      if (!is_string) return ParseError{line_no, "name must be a string"};
      p.name = str;
    } else if (table == "movement" || table == "buttons") {
      if (table == "movement" && key == "target") {
        auto t = is_string ? ParseMovementTarget(str) : std::nullopt;
        if (!t) return ParseError{line_no, "target must be \"dpad\", \"stick\" or \"both\""};
        p.movement_target = *t;
      } else if (table == "movement" && (key == "socd_horizontal" || key == "socd_vertical")) {
        auto policy = is_string ? ParseSocdPolicy(str) : std::nullopt;
        if (!policy) return ParseError{line_no, key + " must be a policy name"};
        if (key == "socd_horizontal" &&
            (*policy == SocdPolicy::kUpPriority || *policy == SocdPolicy::kDownPriority)) {
          return ParseError{line_no, "socd_horizontal cannot be \"up\" or \"down\""};
        }
        (key == "socd_horizontal" ? p.socd_horizontal : p.socd_vertical) = *policy;
      } else {
        auto f = ParseFunction(key);
        if (!f || IsDirection(*f) != (table == "movement")) {
          return ParseError{line_no, "unknown key '" + key + "' in [" + table + "]"};
        }
        if (!is_string) return ParseError{line_no, key + " must be a string of key names"};
        p.keys[static_cast<std::size_t>(*f)] = SplitKeys(str);
      }
    } else if (table == "layout") {
      auto f = ParseFunction(key);
      if (!f || !HasLayout(*f)) return ParseError{line_no, "unknown key '" + key + "' in [layout]"};
      auto control = is_string ? ParsePadControl(str) : std::nullopt;
      if (!control) return ParseError{line_no, key + " must name a pad control (A, B, X, Y, LB, RB, LT, RT, Start, Back, LS, RS)"};
      p.layout[static_cast<std::size_t>(*f)] = *control;
    } else {  // options
      if (key == "latch_presses") {
        if (value == "true") p.latch_presses = true;
        else if (value == "false") p.latch_presses = false;
        else return ParseError{line_no, "latch_presses must be true or false"};
      } else if (key == "player") {
        int player = 0;
        const char* begin = value.data();
        const char* end = begin + value.size();
        auto res = std::from_chars(begin, end, player);
        if (res.ec != std::errc{} || res.ptr != end || player < 1 || player > 4) {
          return ParseError{line_no, "player must be a number from 1 to 4"};
        }
        p.player = player;
      } else {
        return ParseError{line_no, "unknown key '" + key + "' in [options]"};
      }
    }
  }
  out = std::move(p);
  return std::nullopt;
}

std::vector<Conflict> FindConflicts(const Profile& profile) {
  std::vector<Conflict> conflicts;
  std::vector<std::pair<std::string, std::string>> seen;  // normalised -> original spelling, first seen
  for (std::size_t i = 0; i < kFunctionCount; ++i) {
    for (const std::string& key : profile.keys[i]) {
      const std::string norm = NormalizeKeyName(key);
      auto it = std::find_if(conflicts.begin(), conflicts.end(),
                             [&](const Conflict& c) { return NormalizeKeyName(c.key) == norm; });
      if (it != conflicts.end()) {
        it->functions.push_back(static_cast<Function>(i));
        continue;
      }
      conflicts.push_back(Conflict{key, {static_cast<Function>(i)}});
    }
  }
  std::erase_if(conflicts, [](const Conflict& c) { return c.functions.size() < 2; });
  return conflicts;
}

KeyboardPad::KeyboardPad(const Profile& profile) { SetProfile(profile); }

void KeyboardPad::SetProfile(const Profile& profile) {
  profile_ = profile;
  key_index_.clear();
  keys_.clear();
  for (auto& b : bound_) b.clear();
  for (std::size_t i = 0; i < kFunctionCount; ++i) {
    for (const std::string& key : profile_.keys[i]) {
      const std::string norm = NormalizeKeyName(key);
      if (norm.empty()) continue;
      auto it = key_index_.find(norm);
      if (it == key_index_.end()) {
        it = key_index_.emplace(norm, keys_.size()).first;
        keys_.emplace_back();
      }
      bound_[i].push_back(it->second);
    }
  }
  sequence_ = 0;
  last_input_ = {};
}

bool KeyboardPad::KeyDown(std::string_view key, std::uint64_t time_us) {
  auto it = key_index_.find(NormalizeKeyName(key));
  if (it == key_index_.end()) return false;
  KeyState& k = keys_[it->second];
  if (k.down) return true;  // OS key repeat
  k.down = true;
  k.latched = true;
  k.sequence = ++sequence_;
  k.pressed_at_us = time_us;
  k.age_sampled = false;
  return true;
}

bool KeyboardPad::KeyUp(std::string_view key, std::uint64_t /*time_us*/) {
  auto it = key_index_.find(NormalizeKeyName(key));
  if (it == key_index_.end()) return false;
  keys_[it->second].down = false;  // a latched press stays reported until the next poll
  return true;
}

void KeyboardPad::ReleaseAll() {
  for (KeyState& k : keys_) k = {};
}

bool KeyboardPad::Active(std::size_t key) const {
  const KeyState& k = keys_[key];
  return k.down || (profile_.latch_presses && k.latched);
}

bool KeyboardPad::FunctionActive(Function f) const {
  for (std::size_t key : bound_[static_cast<std::size_t>(f)]) {
    if (Active(key)) return true;
  }
  return false;
}

std::uint64_t KeyboardPad::FunctionSequence(Function f, bool newest) const {
  std::uint64_t best = newest ? 0 : UINT64_MAX;
  for (std::size_t key : bound_[static_cast<std::size_t>(f)]) {
    if (!Active(key)) continue;
    const std::uint64_t s = keys_[key].sequence;
    best = newest ? std::max(best, s) : std::min(best, s);
  }
  return best;
}

KeyboardPad::Resolved KeyboardPad::ResolveDirections() const {
  Resolved r;
  r.left = FunctionActive(Function::kLeft);
  r.right = FunctionActive(Function::kRight);
  r.up = FunctionActive(Function::kUp);
  r.down = FunctionActive(Function::kDown);
  // Resolves one axis held in both directions: `a` is left or up, `b` is right or down.
  auto resolve = [this](bool& a, bool& b, Function fa, Function fb, SocdPolicy policy) {
    if (!(a && b)) return;
    switch (policy) {
      case SocdPolicy::kNeutral:
        a = b = false;
        break;
      case SocdPolicy::kLastInput:
      case SocdPolicy::kFirstInput: {
        const bool newest = policy == SocdPolicy::kLastInput;
        const std::uint64_t sa = FunctionSequence(fa, newest);
        const std::uint64_t sb = FunctionSequence(fb, newest);
        const bool keep_a = newest ? (sa > sb) : (sa < sb);
        a = keep_a;
        b = !keep_a;
        break;
      }
      case SocdPolicy::kUpPriority:  // keep `a` (up)
        b = false;
        break;
      case SocdPolicy::kDownPriority:  // keep `b` (down)
        a = false;
        break;
    }
  };
  // The parser rejects up/down priorities for the horizontal axis; a profile built in code falls back to neutral.
  const SocdPolicy horizontal = (profile_.socd_horizontal == SocdPolicy::kUpPriority ||
                                 profile_.socd_horizontal == SocdPolicy::kDownPriority)
                                    ? SocdPolicy::kNeutral
                                    : profile_.socd_horizontal;
  resolve(r.left, r.right, Function::kLeft, Function::kRight, horizontal);
  resolve(r.up, r.down, Function::kUp, Function::kDown, profile_.socd_vertical);
  return r;
}

void KeyboardPad::ApplyControl(PadControl control, PadState& state) const {
  switch (control) {
    case PadControl::kA: state.buttons |= pad::kA; break;
    case PadControl::kB: state.buttons |= pad::kB; break;
    case PadControl::kX: state.buttons |= pad::kX; break;
    case PadControl::kY: state.buttons |= pad::kY; break;
    case PadControl::kLB: state.buttons |= pad::kLB; break;
    case PadControl::kRB: state.buttons |= pad::kRB; break;
    case PadControl::kStart: state.buttons |= pad::kStart; break;
    case PadControl::kBack: state.buttons |= pad::kBack; break;
    case PadControl::kLS: state.buttons |= pad::kLeftThumb; break;
    case PadControl::kRS: state.buttons |= pad::kRightThumb; break;
    case PadControl::kLT: state.left_trigger = 0xFF; break;
    case PadControl::kRT: state.right_trigger = 0xFF; break;
    case PadControl::kCount: break;
  }
}

PadState KeyboardPad::Poll(std::uint64_t now_us) {
  PadState state;
  const Resolved dir = ResolveDirections();
  if (profile_.movement_target != MovementTarget::kStick) {
    if (dir.up) state.buttons |= pad::kDpadUp;
    if (dir.down) state.buttons |= pad::kDpadDown;
    if (dir.left) state.buttons |= pad::kDpadLeft;
    if (dir.right) state.buttons |= pad::kDpadRight;
  }
  if (profile_.movement_target != MovementTarget::kDpad) {
    state.thumb_lx = dir.left ? -pad::kStickMax : dir.right ? pad::kStickMax : 0;
    state.thumb_ly = dir.down ? -pad::kStickMax : dir.up ? pad::kStickMax : 0;
  }
  auto apply = [&](Function f) {
    if (FunctionActive(f)) ApplyControl(profile_.layout[static_cast<std::size_t>(f)], state);
  };
  for (Function f : {Function::kLP, Function::kRP, Function::kLK, Function::kRK, Function::kTag, Function::kStart,
                     Function::kBack}) {
    apply(f);
  }
  if (FunctionActive(Function::kLPRP)) {
    ApplyControl(profile_.layout[static_cast<std::size_t>(Function::kLP)], state);
    ApplyControl(profile_.layout[static_cast<std::size_t>(Function::kRP)], state);
  }
  if (FunctionActive(Function::kLKRK)) {
    ApplyControl(profile_.layout[static_cast<std::size_t>(Function::kLK)], state);
    ApplyControl(profile_.layout[static_cast<std::size_t>(Function::kRK)], state);
  }

  last_input_.buttons = static_cast<std::uint16_t>(
      state.buttons & ~(pad::kDpadUp | pad::kDpadDown | pad::kDpadLeft | pad::kDpadRight));
  last_input_.dx = static_cast<std::int8_t>(dir.right ? 1 : dir.left ? -1 : 0);
  last_input_.dy = static_cast<std::int8_t>(dir.up ? 1 : dir.down ? -1 : 0);

  // Press ages and the latch: a press is sampled by the first poll that reports it.
  for (std::size_t i = 0; i < keys_.size(); ++i) {
    KeyState& k = keys_[i];
    if (Active(i) && !k.age_sampled) {
      const std::uint64_t age = now_us >= k.pressed_at_us ? now_us - k.pressed_at_us : 0;
      ++latency_.samples;
      latency_.total_us += age;
      latency_.max_us = std::max(latency_.max_us, age);
      k.age_sampled = true;
    }
    k.latched = false;
  }
  return state;
}

}  // namespace ttt2::input
