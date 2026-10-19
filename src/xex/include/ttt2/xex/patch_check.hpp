// Pre-check of a title-update patch (.xexp) against a base executable (.xex).
//
// The real compatibility proof happens inside the patcher (the descriptor's image key, decrypted
// with the patched header's key, must equal the base's key), which needs the header delta applied
// first. Everything here is read from the plaintext headers of both files: module flags, title
// IDs, versions, compression types and the descriptor's header windows. It refuses the usual
// mistakes (a TU for another version or title, a patch given as the base, truncated descriptors)
// before any decryption or LZX work, and reports what it could observe. Clean-room code; the
// field meanings come from the public XEX2 documentation and Xenia's patch loader description.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ttt2/xex/xex2.hpp"

namespace ttt2::xex {

struct PatchCompatibility {
  bool compatible = true;
  std::vector<std::string> errors;  ///< reasons the update cannot apply to this base
  std::vector<std::string> notes;   ///< informational findings (digests, media IDs, sizes)
};

/// Checks `patch` against `base` (headers of both parsed already; `base_file` is the complete
/// base file, used only to compute digests for the informational notes).
[[nodiscard]] PatchCompatibility check_patch_compatibility(const Xex2Image& base,
                                                           std::span<const std::uint8_t> base_file,
                                                           const Xex2Image& patch);

}  // namespace ttt2::xex
