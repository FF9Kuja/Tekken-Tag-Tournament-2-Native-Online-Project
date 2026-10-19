// make_validate_fixtures: writes license-free synthetic inputs for the end-to-end tests of
// ttt2-validate. Nothing here comes from a game: the executable is a tiny PE image built in
// memory, the disc image and the title-update packages are produced by the test builders.
//
//   make_validate_fixtures <output-dir>
//
// Produces, under <output-dir>:
//   synthetic.iso        an extracted-partition disc image whose root holds default.xex
//                        (title 4E4D0859, version 0.0.0.2) plus two unrelated files
//   TU_SYNTHETIC         a LIVE package (title 4E4D0859, content type 000B0000) whose Default.xexp
//                        is a delta patch 0.0.0.2 -> 0.0.3.2 that fits the base's headers and
//                        carries the SHA-1 of those headers as its source digest
//   TU_WRONG             the same package but its patch applies to version 0.0.1.2
//   folder/              a console-style Content tree: the TU3 package in its place, a
//                        marketplace package, a title update of another title and a text file
// Exit code 0 when every file was written, 1 otherwise.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pe_image_builder.hpp"
#include "stfs_builder.hpp"
#include "xdvdfs_builder.hpp"
#include "xex_builder.hpp"

#include "ttt2/core/hash.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

namespace fs = std::filesystem;
using ttt2::test::StfsBuilder;
using ttt2::test::XdvdfsBuilder;
using ttt2::test::XexBuilder;

constexpr std::uint32_t kTtt2 = 0x4E4D0859u;
constexpr std::uint32_t kOtherTitle = 0xFFFF0001u;
constexpr std::uint32_t kMediaId = 0x11223344u;
constexpr std::uint32_t kRetail = 0x00000002u;
constexpr std::uint32_t kTu3 = 0x00000302u;
constexpr std::uint32_t kContentTitleUpdate = 0x000B0000u;
constexpr std::uint32_t kContentMarketplace = 0x00000002u;

/// The disc executable: an unencrypted, uncompressed XEX2 around the synthetic PE image.
std::vector<std::uint8_t> make_base_xex() {
  using ttt2::test::kImportTestImageBase;
  using ttt2::test::kImportTestImageSize;
  XexBuilder b;
  b.set_module_flags(0x01 | 0x80);  // TITLE | USER_MODE
  b.add_inline(ttt2::xex::header_key::kEntryPoint, kImportTestImageBase + 0x1000);
  b.add_inline(ttt2::xex::header_key::kImageBaseAddress, kImportTestImageBase);
  b.add_struct(ttt2::xex::header_key::kExecutionInfo,
               XexBuilder::execution_info(kMediaId, kRetail, kRetail, kTtt2, 1, 1, kTtt2));
  b.add_sized(ttt2::xex::header_key::kFileFormatInfo, std::vector<std::uint8_t>(4, 0));  // none / none
  b.add_sized(ttt2::xex::header_key::kOriginalPeName, XexBuilder::sized_string("synthetic.exe"));
  b.add_sized(ttt2::xex::header_key::kImportLibraries,
              XexBuilder::import_libraries(ttt2::test::import_test_libraries()));
  b.set_security(XexBuilder::security_info(static_cast<std::uint32_t>(kImportTestImageSize), 0,
                                           kImportTestImageBase, 0xFFFFFFFFu, 0x2C, {{1, 1}, {2, 1}}));
  b.set_basefile(ttt2::test::make_import_test_image());
  return b.build();
}

/// A title-update patch for `title_id` from `source_version` to Title Update 3's version, whose
/// header window fits the synthetic base and whose source digest is `digest` (20 bytes or null).
std::vector<std::uint8_t> make_patch_xexp(std::uint32_t title_id, std::uint32_t source_version,
                                          const std::uint8_t* digest) {
  const XexBuilder::DeltaDescriptor d{kTu3, source_version, 0x1800, 0x0, 0x100, 0x0};
  XexBuilder b;
  b.set_module_flags(0x10 | 0x40);  // MODULE_PATCH | PATCH_DELTA
  b.add_sized(ttt2::xex::header_key::kDeltaPatchDescriptor, XexBuilder::delta_descriptor(d, digest));
  b.add_struct(ttt2::xex::header_key::kExecutionInfo,
               XexBuilder::execution_info(kMediaId, kTu3, source_version, title_id, 1, 1, title_id));
  b.add_sized(ttt2::xex::header_key::kFileFormatInfo, XexBuilder::file_format_normal(1, 3, 0x8000));
  b.set_security(XexBuilder::security_info(0x100, 0, 0x82000000u, 0xFFFFFFFFu, 0, {}));
  b.set_basefile(std::vector<std::uint8_t>(0x100, 0xDD));
  return b.build();
}

std::vector<std::uint8_t> make_package(std::uint32_t title_id, std::uint32_t content_type,
                                       const std::string& display_name, const std::string& file_name,
                                       std::vector<std::uint8_t> data) {
  StfsBuilder b;
  b.set_title(title_id, content_type, kTu3);
  b.set_display_name(display_name);
  b.add_file(file_name, std::move(data));
  return b.build();
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> v(size);
  for (std::size_t i = 0; i < size; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31);
  return v;
}

bool write(const fs::path& path, const std::vector<std::uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  const bool ok =
      static_cast<bool>(out) && static_cast<bool>(out.write(reinterpret_cast<const char*>(data.data()),
                                                            static_cast<std::streamsize>(data.size())));
  if (ok) {
    std::printf("wrote %s (%zu bytes)\n", path.string().c_str(), data.size());
  } else {
    std::fprintf(stderr, "could not write %s\n", path.string().c_str());
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_validate_fixtures <output-dir>\n");
    return 1;
  }
  const fs::path out = argv[1];

  const auto base = make_base_xex();
  const auto parsed = ttt2::xex::parse(base);
  if (!parsed) {
    std::fprintf(stderr, "the synthetic base does not parse: %s\n", parsed.error().message.c_str());
    return 1;
  }
  const auto header_digest =
      ttt2::hash::Sha1::compute(std::span<const std::uint8_t>(base.data(), parsed->header_size));

  XdvdfsBuilder disc;
  disc.add_file("default.xex", base);
  disc.add_file("readme.txt", pattern(0x120, 7));
  disc.add_file("data/table.bin", pattern(0x3000, 3));

  const auto tu3 = make_package(kTtt2, kContentTitleUpdate, "Title Update #3", "Default.xexp",
                                make_patch_xexp(kTtt2, kRetail, header_digest.data()));
  const auto wrong = make_package(kTtt2, kContentTitleUpdate, "Title Update #3", "Default.xexp",
                                  make_patch_xexp(kTtt2, 0x00000102u, nullptr));
  const auto marketplace =
      make_package(kTtt2, kContentMarketplace, "Costume pack", "content.bin", pattern(0x2000, 9));
  const auto other_title = make_package(kOtherTitle, kContentTitleUpdate, "Other game TU", "Default.xexp",
                                        make_patch_xexp(kOtherTitle, kRetail, nullptr));

  const fs::path content = out / "folder" / "Content" / "0000000000000000";
  bool ok = write(out / "synthetic.iso", disc.build());
  ok = write(out / "TU_SYNTHETIC", tu3) && ok;
  ok = write(out / "TU_WRONG", wrong) && ok;
  ok = write(content / "4E4D0859" / "000B0000" / "TU_SYNTHETIC", tu3) && ok;
  ok = write(content / "4E4D0859" / "00000002" / "DLC_DECOY", marketplace) && ok;
  ok = write(content / "FFFF0001" / "000B0000" / "TU_OTHER", other_title) && ok;
  ok = write(out / "folder" / "notes.txt", pattern(0x40, 1)) && ok;
  return ok ? 0 : 1;
}
