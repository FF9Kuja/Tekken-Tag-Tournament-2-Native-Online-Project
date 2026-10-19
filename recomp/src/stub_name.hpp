// Parses the ReXGlue runtime's stub log lines. Header-only and free of SDK types so the main
// project's unit tests cover it; recomp/src/stub_counter.cpp uses it on the live log stream.
#pragma once

#include <string>
#include <string_view>

namespace ttt2 {

/// The export name reported by a stub log line produced by rex/hook.h ("{} STUB", "{} STUB - {}",
/// "{} STUB - returning {:#x}"): "__imp__NtClose STUB - returning 0x0" -> "NtClose". Empty when the
/// line is not a stub report.
inline std::string stub_name(std::string_view payload) {
  constexpr std::string_view kMarker = " STUB";
  const std::size_t at = payload.find(kMarker);
  if (at == std::string_view::npos || at == 0) return {};
  const std::size_t after = at + kMarker.size();
  if (after < payload.size() && payload[after] != ' ') return {};
  std::string_view name = payload.substr(0, at);
  if (name.find(' ') != std::string_view::npos) return {};
  constexpr std::string_view kPrefix = "__imp__";
  if (name.substr(0, kPrefix.size()) == kPrefix) name.remove_prefix(kPrefix.size());
  return std::string(name);
}

}  // namespace ttt2
