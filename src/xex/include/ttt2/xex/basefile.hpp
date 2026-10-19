// Decryption and decompression of the XEX2 basefile (the PE image inside an
// Xbox 360 executable).
//
// Retail XEX files store the PE image AES-128-CBC encrypted with a per-file
// session key. That key is stored in the security info, itself encrypted
// (AES-128-ECB) with a publicly known retail key. The decrypted stream is
// then either raw ("none" compression) or "basic"-compressed: a list of
// (data_size, zero_size) blocks where the zero runs were simply left out.
// Tekken Tag Tournament 2's retail executable uses normal encryption with
// basic compression (reported by the TTT2 Web project and consistent with
// the XEX headers), so this is the path the pipeline needs first.
//
// "Normal" (LZX) compression and delta patches (.xexp) are not handled here;
// the XenonUtils-based patch tool covers those (Milestone 3).

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ttt2/core/aes.hpp"
#include "ttt2/core/expected.hpp"
#include "ttt2/xex/xex2.hpp"

namespace ttt2::xex {

/// The retail XEX2 key (public; also in Xenia, XenonRecomp and every XEX tool).
inline constexpr aes::Key kRetailKey = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                        0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
/// Development-kit images use an all-zero key.
inline constexpr aes::Key kDevkitKey = {};

struct BasefileOptions {
  aes::Key master_key = kRetailKey;
  /// Refuse images larger than this (a corrupt header could claim gigabytes).
  std::uint32_t max_image_size = 256u * 1024 * 1024;
};

/// The per-file session key: the security info's file key decrypted with the master key.
[[nodiscard]] aes::Key derive_session_key(const SecurityInfo& security,
                                          const aes::Key& master_key = kRetailKey) noexcept;

/// Produces the decrypted, decompressed PE image (`image_size` bytes) of a
/// parsed XEX2 whose file contents are `file`. Supports encryption none/normal
/// and compression none/basic; anything else is an error.
[[nodiscard]] Expected<std::vector<std::uint8_t>> load_basefile(const Xex2Image& image,
                                                                std::span<const std::uint8_t> file,
                                                                const BasefileOptions& options = {});

/// Expands basic-compressed data: for each block, copy `data_size` bytes then emit
/// `zero_size` zeros. Output is sized `image_size`; blocks past it are an error.
[[nodiscard]] Expected<std::vector<std::uint8_t>> decompress_basic(
    std::span<const std::uint8_t> compressed, const std::vector<BasicCompressionBlock>& blocks,
    std::uint32_t image_size);

/// True when the buffer starts with an MZ header whose e_lfanew points at a PE signature.
[[nodiscard]] bool looks_like_pe_image(std::span<const std::uint8_t> image) noexcept;

}  // namespace ttt2::xex
