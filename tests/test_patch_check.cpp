#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "xex_builder.hpp"

#include "ttt2/core/hash.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/patch_check.hpp"
#include "ttt2/xex/xex2.hpp"

using namespace ttt2;
using namespace ttt2::xex;
using ttt2::test::XexBuilder;

namespace {

std::vector<std::uint8_t> make_base(std::uint32_t title_id = 0x4E4D0859u, std::uint32_t version = 0x00000002u,
                                    std::uint32_t media_id = 0x11223344u) {
  XexBuilder b;
  b.set_module_flags(0x01 | 0x80);
  b.add_inline(header_key::kEntryPoint, 0x82110000u);
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(media_id, version, version, title_id, 1, 1, 0));
  b.add_sized(header_key::kFileFormatInfo, XexBuilder::file_format_basic(1, {{0x1000, 0x1000}}));
  b.set_security(XexBuilder::security_info(0x2000, 0, 0x82000000u, 0xFFFFFFFFu, 0x2C, {{1, 1}}));
  b.set_basefile(std::vector<std::uint8_t>(0x1000, 0xEE));
  return b.build();
}

std::vector<std::uint8_t> make_patch(const XexBuilder::DeltaDescriptor& d,
                                     std::uint32_t title_id = 0x4E4D0859u,
                                     const std::uint8_t* digest = nullptr, std::uint16_t compression = 3,
                                     std::uint32_t media_id = 0x11223344u) {
  XexBuilder b;
  b.set_module_flags(0x10 | 0x40);
  b.add_sized(header_key::kDeltaPatchDescriptor, XexBuilder::delta_descriptor(d, digest));
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(media_id, d.target_version, d.source_version, title_id, 1, 1, 0));
  b.add_sized(header_key::kFileFormatInfo, XexBuilder::file_format_normal(1, compression, 0x8000));
  b.set_security(XexBuilder::security_info(0x100, 0, 0x82000000u, 0xFFFFFFFFu, 0, {}));
  b.set_basefile(std::vector<std::uint8_t>(0x100, 0xDD));
  return b.build();
}

// The synthetic base header is a few hundred bytes, so the header window stays within it.
const XexBuilder::DeltaDescriptor kGoodDescriptor{0x00000302u, 0x00000002u, 0x1800, 0x0, 0x100, 0x0};

}  // namespace

TEST_CASE("delta descriptor windows are parsed") {
  const auto patch = parse(make_patch(kGoodDescriptor));
  REQUIRE(patch.has_value());
  REQUIRE(patch->delta_patch.has_value());
  const auto& d = *patch->delta_patch;
  CHECK(d.has_delta_windows);
  CHECK(d.descriptor_size == 0x4C);
  CHECK(d.target_version == kTtt2TitleUpdate3Version);
  CHECK(d.source_version == kTtt2RetailBaseVersion);
  CHECK(d.size_of_target_headers == 0x1800);
  CHECK(d.delta_headers_source_size == 0x100);
}

TEST_CASE("a matching base and title update pass the pre-check") {
  const auto base_file = make_base();
  const auto base = parse(base_file);
  const auto patch = parse(make_patch(kGoodDescriptor));
  REQUIRE(base.has_value());
  REQUIRE(patch.has_value());
  const auto r = check_patch_compatibility(*base, base_file, *patch);
  CHECK(r.compatible);
  CHECK(r.errors.empty());
  REQUIRE_FALSE(r.notes.empty());
  bool mentions_versions = false;
  for (const auto& n : r.notes) mentions_versions |= n.find("0.0.0.2 -> 0.0.3.2") != std::string::npos;
  CHECK(mentions_versions);
}

TEST_CASE("the source digest note identifies what it matches") {
  const auto base_file = make_base();
  const auto base = parse(base_file);
  REQUIRE(base.has_value());
  const auto header_digest =
      hash::Sha1::compute(std::span<const std::uint8_t>(base_file.data(), base->header_size));
  const auto patch = parse(make_patch(kGoodDescriptor, 0x4E4D0859u, header_digest.data()));
  REQUIRE(patch.has_value());
  const auto r = check_patch_compatibility(*base, base_file, *patch);
  CHECK(r.compatible);
  bool found = false;
  for (const auto& n : r.notes) found |= n.find("SHA-1 of the base's headers") != std::string::npos;
  CHECK(found);
}

TEST_CASE("mismatches are refused with a reason") {
  const auto base_file = make_base();
  const auto base = parse(base_file);
  REQUIRE(base.has_value());
  SUBCASE("wrong base version") {
    const auto other_base_file = make_base(0x4E4D0859u, 0x00000102u);
    const auto other_base = parse(other_base_file);
    const auto patch = parse(make_patch(kGoodDescriptor));
    const auto r = check_patch_compatibility(*other_base, other_base_file, *patch);
    CHECK_FALSE(r.compatible);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("version mismatch") != std::string::npos);
    CHECK(r.errors[0].find("0.0.1.2") != std::string::npos);
  }
  SUBCASE("wrong title") {
    const auto patch = parse(make_patch(kGoodDescriptor, 0x4E4D07FCu));
    const auto r = check_patch_compatibility(*base, base_file, *patch);
    CHECK_FALSE(r.compatible);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("title ID mismatch") != std::string::npos);
  }
  SUBCASE("header window outside the base headers") {
    XexBuilder::DeltaDescriptor d = kGoodDescriptor;
    d.headers_source_offset = 0x10000;
    const auto patch = parse(make_patch(d));
    const auto r = check_patch_compatibility(*base, base_file, *patch);
    CHECK_FALSE(r.compatible);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("beyond the base header size") != std::string::npos);
  }
  SUBCASE("header window larger than the base headers") {
    XexBuilder::DeltaDescriptor d = kGoodDescriptor;
    d.headers_source_size = 0x10000;
    const auto patch = parse(make_patch(d));
    const auto r = check_patch_compatibility(*base, base_file, *patch);
    CHECK_FALSE(r.compatible);
    CHECK(r.errors.size() == 2);  // past the base header and does not fit the target header size
  }
  SUBCASE("the patch is not delta-compressed") {
    const auto patch = parse(make_patch(kGoodDescriptor, 0x4E4D0859u, nullptr, 2));
    const auto r = check_patch_compatibility(*base, base_file, *patch);
    CHECK_FALSE(r.compatible);
    CHECK(r.errors[0].find("not delta") != std::string::npos);
  }
  SUBCASE("files swapped") {
    const auto patch_file = make_patch(kGoodDescriptor);
    const auto patch = parse(patch_file);
    const auto r = check_patch_compatibility(*patch, patch_file, *base);
    CHECK_FALSE(r.compatible);
    CHECK(r.errors.size() >= 2);
  }
  SUBCASE("different media IDs are only a note") {
    const auto patch = parse(make_patch(kGoodDescriptor, 0x4E4D0859u, nullptr, 3, 0x55667788u));
    const auto r = check_patch_compatibility(*base, base_file, *patch);
    CHECK(r.compatible);
    bool noted = false;
    for (const auto& n : r.notes) noted |= n.find("media ID differs") != std::string::npos;
    CHECK(noted);
  }
}
