#include <doctest.h>

#include <cstdint>
#include <vector>

#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"
#include "xex_builder.hpp"

using namespace ttt2;
using namespace ttt2::xex;
using ttt2::test::XexBuilder;

namespace {

/// A synthetic title executable that exercises every header the parser decodes.
std::vector<std::uint8_t> make_title_image() {
  XexBuilder b;
  b.set_module_flags(0x01 | 0x80);  // TITLE | USER_MODE
  b.add_inline(header_key::kEntryPoint, 0x82110000u);
  b.add_inline(header_key::kImageBaseAddress, 0x82000000u);
  b.add_inline(header_key::kOriginalBaseAddress, 0x82000000u);
  b.add_inline(header_key::kDefaultStackSize, 0x00040000u);
  b.add_inline(header_key::kSystemFlags, 0x00000021u);
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(0x12345678u, 0x00000302u, 0x00000002u, 0x4E4D0859u, 1, 1, 0x4E4D0859u));
  b.add_sized(header_key::kFileFormatInfo, XexBuilder::file_format_basic(1, {{0x1000, 0x0}, {0x2000, 0x3000}}));
  b.add_sized(header_key::kOriginalPeName, XexBuilder::sized_string("tekken.exe"));
  b.add_sized(header_key::kImportLibraries,
              XexBuilder::import_libraries({{"xboxkrnl.exe", 0, 0x00000302u, 0x00000301u, 3},
                                            {"xam.xex", 1, 0x00000302u, 0x00000301u, 2}}));
  b.add_sized(header_key::kStaticLibraries,
              XexBuilder::static_libraries({{"XAPILIB", 2, 0, 20353, 0}, {"D3D9", 2, 0, 20353, 0}}));
  b.add_struct(header_key::kTlsInfo, [] {
    std::vector<std::uint8_t> t(0x10, 0);
    store_be32(t.data(), 64);
    store_be32(t.data() + 4, 0x82800000u);
    store_be32(t.data() + 8, 0x100);
    store_be32(t.data() + 12, 0x80);
    return t;
  }());
  b.add_struct(header_key::kChecksumTimestamp, [] {
    std::vector<std::uint8_t> t(8, 0);
    store_be32(t.data(), 0xAABBCCDDu);
    store_be32(t.data() + 4, 0x50000000u);
    return t;
  }());
  b.set_security(XexBuilder::security_info(0x00C00000u, 0x10000000u, 0x82000000u, 0x00FF00FFu, 0x0000002Cu,
                                           {{1, 0x100}, {2, 0x40}, {3, 0x20}}));
  b.set_basefile(std::vector<std::uint8_t>(64, 0xEE));
  return b.build();
}

}  // namespace

TEST_CASE("Version packs and unpacks like the XEX2 layout") {
  const Version v = Version::from_u32(0x00000302u);
  CHECK(v == Version{0, 0, 3, 2});
  CHECK(v.to_string() == "0.0.3.2");
  CHECK(v.to_u32() == 0x00000302u);
  const Version big = Version::from_u32(0x2A1F0507u);
  CHECK(big.major == 2);
  CHECK(big.minor == 10);
  CHECK(big.build == 0x1F05);
  CHECK(big.qfe == 7);
  CHECK(big.to_u32() == 0x2A1F0507u);
  CHECK(kTtt2TitleUpdate3Version.to_string() == "0.0.3.2");
}

TEST_CASE("parses a synthetic title executable") {
  const auto image = make_title_image();
  const auto result = parse(image);
  REQUIRE(result.has_value());
  const Xex2Image& img = result.value();

  CHECK(img.is_title());
  CHECK_FALSE(img.is_patch());
  CHECK(img.header_size > 0x18);  // the basefile starts after the headers
  CHECK(img.header_size % 0x10 == 0);
  CHECK(img.optional_headers.size() == 12);
  CHECK(img.warnings.empty());

  CHECK(img.entry_point == 0x82110000u);
  CHECK(img.image_base == 0x82000000u);
  CHECK(img.original_base_address == 0x82000000u);
  CHECK(img.default_stack_size == 0x40000u);
  CHECK(img.system_flags == 0x21u);

  REQUIRE(img.execution_info.has_value());
  CHECK(img.execution_info->title_id == 0x4E4D0859u);
  CHECK(img.execution_info->media_id == 0x12345678u);
  CHECK(img.execution_info->version.to_string() == "0.0.3.2");
  CHECK(img.execution_info->base_version.to_string() == "0.0.0.2");
  CHECK(img.execution_info->disc_number == 1);
  CHECK(img.execution_info->disc_count == 1);
  CHECK(img.execution_info->savegame_id == 0x4E4D0859u);
  CHECK(known_title_name(img.execution_info->title_id) == std::string_view("Tekken Tag Tournament 2"));

  REQUIRE(img.file_format.has_value());
  CHECK(img.file_format->encryption == EncryptionType::Normal);
  CHECK(img.file_format->compression == CompressionType::Basic);
  REQUIRE(img.file_format->basic_blocks.size() == 2);
  CHECK(img.file_format->basic_blocks[1].data_size == 0x2000);
  CHECK(img.file_format->basic_blocks[1].zero_size == 0x3000);

  CHECK(img.original_pe_name == std::string("tekken.exe"));

  REQUIRE(img.import_libraries.size() == 2);
  CHECK(img.import_libraries[0].name == "xboxkrnl.exe");
  CHECK(img.import_libraries[0].import_count == 3);
  CHECK(img.import_libraries[0].version.to_string() == "0.0.3.2");
  CHECK(img.import_libraries[0].version_min.to_string() == "0.0.3.1");
  CHECK(img.import_libraries[1].name == "xam.xex");
  CHECK(img.import_libraries[1].id == 1);
  CHECK(img.import_libraries[1].import_count == 2);

  REQUIRE(img.static_libraries.size() == 2);
  CHECK(img.static_libraries[0].name == "XAPILIB");
  CHECK(img.static_libraries[0].version_string() == "2.0.20353.0");
  CHECK(img.static_libraries[1].name == "D3D9");

  REQUIRE(img.tls_info.has_value());
  CHECK(img.tls_info->slot_count == 64);
  CHECK(img.tls_info->raw_data_address == 0x82800000u);
  REQUIRE(img.checksum_timestamp.has_value());
  CHECK(img.checksum_timestamp->first == 0xAABBCCDDu);
  CHECK(img.checksum_timestamp->second == 0x50000000u);

  REQUIRE(img.security.has_value());
  CHECK(img.security->image_size == 0x00C00000u);
  CHECK(img.security->image_flags == 0x10000000u);
  CHECK(img.security->load_address == 0x82000000u);
  CHECK(img.security->region == 0x00FF00FFu);
  CHECK(img.security->allowed_media_types == 0x2Cu);
  CHECK(img.security->page_descriptor_count == 3);
  REQUIRE(img.security->page_descriptors.size() == 3);
  CHECK(img.security->page_descriptors[0].info == 1);
  CHECK(img.security->page_descriptors[0].page_count == 0x100);
  CHECK(img.security->page_descriptors[2].info == 3);
  CHECK(img.security->page_descriptors[2].page_count == 0x20);
  CHECK(img.security->file_key[0] == 0x55);
  CHECK(img.security->header_digest[19] == 0x66);

  CHECK(img.find_header(header_key::kEntryPoint) != nullptr);
  CHECK(img.find_header(header_key::kLanKey) == nullptr);
  CHECK(optional_header_name(header_key::kExecutionInfo) == std::string_view("EXECUTION_INFO"));
  CHECK(optional_header_name(0x12345678u) == std::string_view("UNKNOWN"));
  CHECK(describe_module_flags(img.module_flags) == "TITLE | USER_MODE");
  CHECK(describe_region(0xFFFFFFFFu) == "ALL");
}

TEST_CASE("parses a synthetic delta patch (.xexp)") {
  XexBuilder b;
  b.set_module_flags(0x10 | 0x40);  // MODULE_PATCH | PATCH_DELTA
  std::vector<std::uint8_t> desc(0x4C - 4, 0);  // body without the size prefix
  store_be32(desc.data() + 0x0, 0x00000302u);   // target 0.0.3.2
  store_be32(desc.data() + 0x4, 0x00000002u);   // source 0.0.0.2
  for (int i = 0; i < 0x14; ++i) desc[0x8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  store_be32(desc.data() + 0x2C, 0x1000);       // size of target headers
  b.add_sized(header_key::kDeltaPatchDescriptor, desc);
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(0, 0x00000302u, 0x00000002u, 0x4E4D0859u, 1, 1, 0));
  b.set_security(XexBuilder::security_info(0x1000, 0, 0x82000000u, 0xFFFFFFFFu, 0, {}));
  const auto image = b.build();

  const auto result = parse(image);
  REQUIRE(result.has_value());
  const auto& img = result.value();
  CHECK(img.is_patch());
  CHECK(img.is_delta_patch());
  CHECK_FALSE(img.is_title());
  REQUIRE(img.delta_patch.has_value());
  CHECK(img.delta_patch->target_version == kTtt2TitleUpdate3Version);
  CHECK(img.delta_patch->source_version == kTtt2RetailBaseVersion);
  CHECK(img.delta_patch->source_digest[0x13] == 0x13);
  CHECK(img.delta_patch->size_of_target_headers == 0x1000);
  CHECK(img.warnings.empty());
}

TEST_CASE("rejects non-XEX input") {
  const std::vector<std::uint8_t> empty;
  CHECK_FALSE(parse(empty).has_value());
  std::vector<std::uint8_t> garbage(0x100, 0x41);
  CHECK_FALSE(parse(garbage).has_value());
  CHECK(parse(garbage).error().message.find("magic") != std::string::npos);
  auto pe = garbage;
  pe[0] = 'M';
  pe[1] = 'Z';
  CHECK_FALSE(parse(pe).has_value());
}

TEST_CASE("implausible header counts and out-of-range offsets are handled") {
  auto image = make_title_image();
  // Header count far beyond the file.
  store_be32(image.data() + 0x14, 0x7FFFFFFFu);
  CHECK_FALSE(parse(image).has_value());
  // Count within the limit but table past the end of the file.
  store_be32(image.data() + 0x14, 1000);
  CHECK_FALSE(parse(image).has_value());

  image = make_title_image();
  // Point the execution info header at an impossible offset: must warn, not crash.
  for (std::size_t i = 0; i < 12; ++i) {
    const std::size_t e = 0x18 + i * 8;
    if (load_be32(image.data() + e) == header_key::kExecutionInfo) store_be32(image.data() + e + 4, 0xFFFFFF00u);
  }
  const auto result = parse(image);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->execution_info.has_value());
  CHECK_FALSE(result->warnings.empty());
}

TEST_CASE("every truncation of a valid image is handled without crashing") {
  const auto image = make_title_image();
  for (std::size_t len = 0; len < image.size(); len += 7) {
    const std::span<const std::uint8_t> view(image.data(), len);
    const auto result = parse(view);  // either an error or a partial parse with warnings
    if (result.has_value()) {
      CHECK(result->optional_headers.size() <= 12);
    }
  }
  // Also with every single byte flipped in the header area: no crash allowed.
  for (std::size_t i = 0; i < 0x18 + 12 * 8; ++i) {
    auto mutated = image;
    mutated[i] ^= 0xFF;
    (void)parse(mutated);
  }
}

TEST_CASE("known executable lookup") {
  CHECK(match_known_executable("a942090b7c293d0ce63346ed8a2974448135d0dd", "") != nullptr);
  CHECK(match_known_executable("", "ee138a4fe8d32a0edc131451377f4377041a49626153fa992db24f5420a7f15c") != nullptr);
  CHECK(match_known_executable("", "") == nullptr);
  CHECK(match_known_executable("0000000000000000000000000000000000000000", "") == nullptr);
  CHECK(known_title_name(0x4E4D07FCu) == std::string_view("Tekken 6"));
  CHECK_FALSE(known_title_name(0x12345678u).has_value());
}
