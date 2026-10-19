#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pe_image_builder.hpp"
#include "xex_builder.hpp"

#include "ttt2/core/endian.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/imports.hpp"
#include "ttt2/xex/xex2.hpp"

using namespace ttt2;
using namespace ttt2::xex;
using ttt2::test::XexBuilder;

namespace {

constexpr std::uint32_t kBase = ttt2::test::kImportTestImageBase;
constexpr std::size_t kImageSize = ttt2::test::kImportTestImageSize;

std::vector<std::uint8_t> make_image() {
  return ttt2::test::make_import_test_image();
}

std::vector<std::uint8_t> make_xex(const std::vector<std::uint8_t>& image) {
  XexBuilder b;
  b.add_inline(header_key::kEntryPoint, kBase + 0x1000);
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(1, 0x00000002u, 0x00000002u, 0x4E4D0859u, 1, 1, 0));
  b.add_sized(header_key::kFileFormatInfo, [] {
    std::vector<std::uint8_t> f(4, 0);  // encryption none, compression none
    return f;
  }());
  b.add_sized(header_key::kImportLibraries,
              XexBuilder::import_libraries(ttt2::test::import_test_libraries()));
  b.set_security(XexBuilder::security_info(kImageSize, 0, kBase, 0xFFFFFFFFu, 0, {{1, 1}, {2, 1}}));
  b.set_basefile(image);
  return b.build();
}

}  // namespace

TEST_CASE("export tables name kernel and XAM ordinals") {
  const auto nt_close = find_export("xboxkrnl.exe", 207);
  REQUIRE(nt_close.has_value());
  CHECK(std::string(nt_close->name) == "NtClose");
  CHECK_FALSE(nt_close->variable);
  const auto bundle = find_export("XBOXKRNL", 173);
  REQUIRE(bundle.has_value());
  CHECK(std::string(bundle->name) == "KeTimeStampBundle");
  CHECK(bundle->variable);
  const auto signin = find_export("xam.xex", 700);
  REQUIRE(signin.has_value());
  CHECK(std::string(signin->name) == "XamShowSigninUI");
  CHECK_FALSE(find_export("xam.xex", 0xFFF0).has_value());
  CHECK_FALSE(find_export("other.xex", 1).has_value());
}

TEST_CASE("import records are parsed from the table and resolved against the image") {
  const auto image = make_image();
  const auto file = make_xex(image);
  const auto parsed = parse(file);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->import_libraries.size() == 2);
  CHECK(parsed->import_libraries[0].record_addresses.size() == 4);
  CHECK(parsed->import_libraries[0].record_addresses[1] == kBase + 0x1100);
  CHECK(parsed->import_libraries[1].import_count == 3);

  const auto loaded = load_basefile(*parsed, file);
  REQUIRE(loaded.has_value());
  CHECK(*loaded == image);

  const auto summary = resolve_imports(*parsed, *loaded);
  REQUIRE(summary.has_value());
  const auto& im = *summary;
  REQUIRE(im.entries.size() == 4);
  CHECK(im.functions == 2);
  CHECK(im.variables == 2);
  CHECK(im.unnamed == 1);
  CHECK(im.entries[0].name == "NtClose");
  CHECK(im.entries[0].is_function);
  CHECK(im.entries[0].slot_address == kBase + 0x1800);
  CHECK(im.entries[0].thunk_address == kBase + 0x1100);
  CHECK(im.entries[1].name == "KeTimeStampBundle");
  CHECK_FALSE(im.entries[1].is_function);
  CHECK(im.entries[1].known_variable);
  CHECK(im.entries[2].library == "xam.xex");
  CHECK(im.entries[2].name == "XamShowSigninUI");
  CHECK(im.entries[2].thunk_address == kBase + 0x1110);
  CHECK(im.entries[3].name.empty());
  CHECK(im.entries[3].ordinal == 0xFFF0);
  REQUIRE(im.warnings.size() == 1);  // the record at 0x90000000 lies outside the image
  CHECK(im.warnings[0].find("90000000") != std::string::npos);
}

TEST_CASE("PE sections are read and the helper scan covers only executable ones") {
  const auto image = make_image();
  const auto sections = pe_sections(image);
  REQUIRE(sections.has_value());
  REQUIRE(sections->size() == 2);
  CHECK((*sections)[0].name == ".text");
  CHECK((*sections)[0].rva == 0x1000);
  CHECK((*sections)[0].executable());
  CHECK((*sections)[1].name == ".rdata");
  CHECK_FALSE((*sections)[1].executable());

  const auto hits = scan_save_restore_helpers(image, kBase);
  REQUIRE(hits.size() == 8);
  for (const auto& h : hits) {
    if (h.name == "__savegprlr_14") {
      REQUIRE(h.addresses.size() == 2);
      CHECK(h.addresses[0] == kBase + 0x1200);
      CHECK(h.addresses[1] == kBase + 0x1400);
    } else if (h.name == "__restvmx_64") {
      REQUIRE(h.addresses.size() == 1);
      CHECK(h.addresses[0] == kBase + 0x1300);
    } else {
      CHECK(h.addresses.empty());  // __restgprlr_14 sits in .rdata and must not be reported
    }
  }

  std::vector<std::uint8_t> garbage(0x200, 0x41);
  CHECK_FALSE(pe_sections(garbage).has_value());
  auto truncated = image;
  truncated.resize(0x90);
  CHECK_FALSE(pe_sections(truncated).has_value());
}
