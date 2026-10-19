// A small synthetic PE32 image for tests. No game data: every byte is produced here. Shared by
// the import-resolution unit tests and the fixture generator of the ttt2-validate end-to-end test.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "xex_builder.hpp"

#include "ttt2/core/endian.hpp"

namespace ttt2::test {

inline constexpr std::uint32_t kImportTestImageBase = 0x82000000u;
inline constexpr std::size_t kImportTestImageSize = 0x2000;

inline void store_le32_at(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
  v[off] = static_cast<std::uint8_t>(x);
  v[off + 1] = static_cast<std::uint8_t>(x >> 8);
  v[off + 2] = static_cast<std::uint8_t>(x >> 16);
  v[off + 3] = static_cast<std::uint8_t>(x >> 24);
}

/// A plaintext PE32 image: .text (executable) at RVA 0x1000, .rdata at 0x1800, with import record
/// words and register save/restore helper byte patterns at known places:
///   0x1800 NtClose slot, 0x1100 NtClose thunk, 0x1808 KeTimeStampBundle (variable),
///   0x180C / 0x1110 XamShowSigninUI slot and thunk, 0x1810 an unknown XAM ordinal (0xFFF0);
///   __savegprlr_14 at 0x1200 and 0x1400 (ambiguous), __restvmx_64 at 0x1300,
///   __restgprlr_14 at 0x1900 (in .rdata, so ignored by the scan).
inline std::vector<std::uint8_t> make_import_test_image() {
  std::vector<std::uint8_t> img(kImportTestImageSize, 0);
  img[0] = 'M';
  img[1] = 'Z';
  store_le32_at(img, 0x3C, 0x80);
  std::memcpy(img.data() + 0x80, "PE\0\0", 4);
  img[0x80 + 6] = 2;                    // NumberOfSections
  store_le32_at(img, 0x80 + 20, 0xE0);  // SizeOfOptionalHeader (low 16 bits)
  const std::size_t sect = 0x80 + 24 + 0xE0;
  std::memcpy(img.data() + sect, ".text\0\0\0", 8);
  store_le32_at(img, sect + 8, 0x800);
  store_le32_at(img, sect + 12, 0x1000);
  store_le32_at(img, sect + 16, 0x800);
  store_le32_at(img, sect + 36, 0x60000020u);
  std::memcpy(img.data() + sect + 40, ".rdata\0\0", 8);
  store_le32_at(img, sect + 40 + 8, 0x400);
  store_le32_at(img, sect + 40 + 12, 0x1800);
  store_le32_at(img, sect + 40 + 16, 0x400);
  store_le32_at(img, sect + 40 + 36, 0x40000040u);
  // Import records (big-endian words: type << 24 | ordinal).
  store_be32(img.data() + 0x1800, 0x000000CFu);  // NtClose slot
  store_be32(img.data() + 0x1100, 0x010000CFu);  // NtClose thunk (in .text)
  store_be32(img.data() + 0x1808, 0x000000ADu);  // KeTimeStampBundle (variable)
  store_be32(img.data() + 0x180C, 0x000002BCu);  // XamShowSigninUI slot
  store_be32(img.data() + 0x1110, 0x010002BCu);  // XamShowSigninUI thunk
  store_be32(img.data() + 0x1810, 0x0000FFF0u);  // unknown ordinal
  const std::uint8_t savegprlr[] = {0xF9, 0xC1, 0xFF, 0x68};
  const std::uint8_t restvmx64[] = {0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB};
  const std::uint8_t restgprlr[] = {0xE9, 0xC1, 0xFF, 0x68};
  std::memcpy(img.data() + 0x1200, savegprlr, 4);
  std::memcpy(img.data() + 0x1400, savegprlr, 4);
  std::memcpy(img.data() + 0x1300, restvmx64, 8);
  std::memcpy(img.data() + 0x1900, restgprlr, 4);
  return img;
}

/// The IMPORT_LIBRARIES entries that describe make_import_test_image(): four kernel records (one
/// at an address outside the image) and three XAM records.
inline std::vector<XexBuilder::ImportLib> import_test_libraries() {
  constexpr std::uint32_t kBase = kImportTestImageBase;
  return {{"xboxkrnl.exe",
           0,
           0x00000302u,
           0x00000301u,
           0,
           {kBase + 0x1800, kBase + 0x1100, kBase + 0x1808, 0x90000000u}},
          {"xam.xex", 1, 0x00000302u, 0x00000301u, 0, {kBase + 0x180C, kBase + 0x1110, kBase + 0x1810}}};
}

}  // namespace ttt2::test
