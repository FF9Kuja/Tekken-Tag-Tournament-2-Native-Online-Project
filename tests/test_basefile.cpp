#include <doctest.h>

#include <cstring>
#include <vector>

#include "xex_builder.hpp"

#include "ttt2/core/aes.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/xex2.hpp"

using namespace ttt2;
using namespace ttt2::xex;
using ttt2::test::XexBuilder;

namespace {

/// A fake PE image: MZ header, PE signature at e_lfanew, a zero run in the middle.
std::vector<std::uint8_t> make_pe_image(std::size_t size) {
  std::vector<std::uint8_t> img(size, 0);
  img[0] = 'M';
  img[1] = 'Z';
  const std::uint32_t e_lfanew = 0x80;
  img[0x3C] = static_cast<std::uint8_t>(e_lfanew);
  img[e_lfanew] = 'P';
  img[e_lfanew + 1] = 'E';
  for (std::size_t i = 0x100; i < 0x1000 && i < size; ++i) img[i] = static_cast<std::uint8_t>(i * 13 + 1);
  // [0x1000, 0x2000) stays zero (the run basic compression removes)
  for (std::size_t i = 0x2000; i < size; ++i) img[i] = static_cast<std::uint8_t>(i * 7 + 5);
  return img;
}

struct Fixture {
  std::vector<std::uint8_t> image;  // plaintext PE image
  std::vector<std::uint8_t> file;   // complete synthetic XEX
  aes::Key session_key;
};

/// Builds a synthetic XEX whose basefile is basic-compressed and AES-CBC encrypted
/// exactly as a retail image is (session key stored encrypted with the retail key).
Fixture make_encrypted_fixture(bool encrypt) {
  Fixture f;
  f.image = make_pe_image(0x3000);
  for (std::size_t i = 0; i < 16; ++i)
    f.session_key[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0xA0 + i);

  // Basic compression: block 1 = 0x1000 data + 0x1000 zeros, block 2 = 0x1000 data + 0 zeros.
  std::vector<std::uint8_t> compressed;
  compressed.insert(compressed.end(), f.image.begin(), f.image.begin() + 0x1000);
  compressed.insert(compressed.end(), f.image.begin() + 0x2000, f.image.end());
  const std::vector<std::pair<std::uint32_t, std::uint32_t>> blocks = {{0x1000, 0x1000}, {0x1000, 0}};

  // The stored file key is the session key encrypted with the retail key.
  aes::Key stored_key{};
  const aes::Aes128 retail(kRetailKey);
  retail.encrypt_block(f.session_key.data(), stored_key.data());

  if (encrypt) {
    const aes::Aes128 session(f.session_key);
    session.cbc_encrypt(compressed, aes::Block{});
  }

  XexBuilder b;
  b.add_inline(header_key::kEntryPoint, 0x82010000u);
  b.add_struct(header_key::kExecutionInfo,
               XexBuilder::execution_info(1, 0x00000002u, 0x00000002u, 0x4E4D0859u, 1, 1, 0));
  b.add_sized(header_key::kFileFormatInfo, XexBuilder::file_format_basic(encrypt ? 1 : 0, blocks));
  b.set_security(XexBuilder::security_info(static_cast<std::uint32_t>(f.image.size()), 0, 0x82000000u,
                                           0xFFFFFFFFu, 0, {{1, 3}}, stored_key.data()));
  b.set_basefile(compressed);
  f.file = b.build();
  return f;
}

}  // namespace

TEST_CASE("session key derivation decrypts the stored file key with the retail key") {
  const Fixture f = make_encrypted_fixture(true);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->security.has_value());
  CHECK(derive_session_key(*parsed->security) == f.session_key);
}

TEST_CASE("an encrypted, basic-compressed basefile is recovered byte for byte") {
  const Fixture f = make_encrypted_fixture(true);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  const auto image = load_basefile(*parsed, f.file);
  REQUIRE(image.has_value());
  CHECK(image->size() == f.image.size());
  CHECK(*image == f.image);
  CHECK(looks_like_pe_image(*image));
  CHECK(hash::to_hex(hash::Sha1::compute(*image)) == hash::to_hex(hash::Sha1::compute(f.image)));
}

TEST_CASE("an unencrypted basic-compressed basefile is recovered too") {
  const Fixture f = make_encrypted_fixture(false);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  const auto image = load_basefile(*parsed, f.file);
  REQUIRE(image.has_value());
  CHECK(*image == f.image);
}

TEST_CASE("a wrong master key yields garbage, which is reported as not a PE image") {
  const Fixture f = make_encrypted_fixture(true);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  BasefileOptions options;
  options.master_key = kDevkitKey;
  const auto image = load_basefile(*parsed, f.file, options);
  REQUIRE(image.has_value());  // decryption itself cannot fail
  CHECK_FALSE(looks_like_pe_image(*image));
  CHECK(*image != f.image);
}

TEST_CASE("basic decompression rejects inconsistent block tables") {
  const std::vector<std::uint8_t> data(0x100, 0xAB);
  CHECK_FALSE(decompress_basic(data, {{0x200, 0}}, 0x400).has_value());      // reads past the input
  CHECK_FALSE(decompress_basic(data, {{0x100, 0x400}}, 0x400).has_value());  // writes past the image
  const auto ok = decompress_basic(data, {{0x80, 0x80}, {0x80, 0x80}}, 0x200);
  REQUIRE(ok.has_value());
  CHECK(ok->size() == 0x200);
  CHECK((*ok)[0x7F] == 0xAB);
  CHECK((*ok)[0x80] == 0);
  CHECK((*ok)[0x100] == 0xAB);
}

TEST_CASE("load_basefile refuses what it cannot handle") {
  Fixture f = make_encrypted_fixture(true);
  auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  SUBCASE("implausible image size") {
    parsed->security->image_size = 0x7FFFFFFFu;
    CHECK_FALSE(load_basefile(*parsed, f.file).has_value());
  }
  SUBCASE("normal (LZX) compression") {
    parsed->file_format->compression = CompressionType::Normal;
    const auto r = load_basefile(*parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("LZX") != std::string::npos);
  }
  SUBCASE("delta patch") {
    parsed->file_format->compression = CompressionType::Delta;
    CHECK_FALSE(load_basefile(*parsed, f.file).has_value());
  }
  SUBCASE("truncated file") {
    f.file.resize(parsed->header_size + 0x100);
    const auto r = load_basefile(*parsed, f.file);
    CHECK_FALSE(r.has_value());
  }
}

TEST_CASE("PE sniffing") {
  const auto img = make_pe_image(0x200);
  CHECK(looks_like_pe_image(img));
  std::vector<std::uint8_t> bad(0x200, 0);
  CHECK_FALSE(looks_like_pe_image(bad));
  auto far = img;
  far[0x3C] = 0xFF;
  far[0x3D] = 0xFF;
  CHECK_FALSE(looks_like_pe_image(far));  // e_lfanew outside the buffer
  CHECK_FALSE(looks_like_pe_image(std::span<const std::uint8_t>(img.data(), 0x20)));
}
