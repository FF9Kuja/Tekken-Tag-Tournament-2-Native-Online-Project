// Small formatting helpers shared by the libraries and tools.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ttt2 {

/// Eight upper-case hexadecimal digits, e.g. 0x4E4D0859 -> "4E4D0859".
[[nodiscard]] inline std::string hex_u32(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(v));
  return buf;
}

}  // namespace ttt2
