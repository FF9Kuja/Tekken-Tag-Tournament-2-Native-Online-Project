// Small file helpers shared by the tools.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "ttt2/core/expected.hpp"

namespace ttt2 {

/// Reads a whole file into memory. Refuses files larger than `max_bytes`
/// (default 256 MiB) so a mistaken path to a disc image cannot exhaust RAM.
[[nodiscard]] Expected<std::vector<std::uint8_t>> read_file(
    const std::filesystem::path& path, std::uint64_t max_bytes = 256ull * 1024 * 1024);

}  // namespace ttt2
