// Identification data for the executables this project cares about.
//
// Everything in this file is metadata (IDs, version numbers, hashes). It
// contains no game code or data. Hashes marked "reported" come from other
// public projects and have NOT yet been verified against a copy of the game by
// this project; they are kept here so the tooling can at least say "this looks
// like the copy other ports were built from". See docs/RESEARCH.md.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ttt2/xex/xex2.hpp"

namespace ttt2::xex {

struct KnownTitle {
  std::uint32_t title_id;
  std::string_view name;
};

inline constexpr KnownTitle kKnownTitles[] = {
    {0x4E4D0859u, "Tekken Tag Tournament 2"},
    {0x4E4D07FCu, "Tekken 6"},
};

struct KnownExecutable {
  std::uint32_t title_id;
  std::string_view description;
  std::string_view sha1_hex;    ///< empty if unknown
  std::string_view sha256_hex;  ///< empty if unknown
  std::string_view provenance;  ///< where the hash was published
};

inline constexpr KnownExecutable kKnownExecutables[] = {
    // Reported by the TTT2Recomp project (shihabal3amri/tekken-tag-tournament-2-pc,
    // port/src/setup/game_data.cpp, "retail 0.0.0.2"). Unverified by this project.
    {0x4E4D0859u, "Tekken Tag Tournament 2 (USA, Europe) retail disc Default.xex, v0.0.0.2",
     "a942090b7c293d0ce63346ed8a2974448135d0dd", "",
     "reported by TTT2Recomp (game_data.cpp); not yet verified here"},
    // Reported by Tekken 6 Native Online (Ekspym, builder/build_native.py). Unverified here.
    {0x4E4D07FCu, "Tekken 6 (USA, Europe) Rev 1 Default.xex", "",
     "ee138a4fe8d32a0edc131451377f4377041a49626153fa992db24f5420a7f15c",
     "reported by Tekken 6 Native Online (build_native.py); not yet verified here"},
};

/// Title Update 3 for Tekken Tag Tournament 2 targets version 0.0.3.2 and is
/// the update the existing PC port builds against (final balance patch).
inline constexpr Version kTtt2TitleUpdate3Version{0, 0, 3, 2};
inline constexpr Version kTtt2RetailBaseVersion{0, 0, 0, 2};

[[nodiscard]] inline std::optional<std::string_view> known_title_name(std::uint32_t title_id) noexcept {
  for (const auto& t : kKnownTitles) {
    if (t.title_id == title_id) return t.name;
  }
  return std::nullopt;
}

/// Finds a known executable by its SHA-1 or SHA-256 (lower-case hex).
[[nodiscard]] inline const KnownExecutable* match_known_executable(std::string_view sha1_hex,
                                                                   std::string_view sha256_hex) noexcept {
  for (const auto& e : kKnownExecutables) {
    if (!e.sha1_hex.empty() && e.sha1_hex == sha1_hex) return &e;
    if (!e.sha256_hex.empty() && e.sha256_hex == sha256_hex) return &e;
  }
  return nullptr;
}

}  // namespace ttt2::xex
