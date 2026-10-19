#include <doctest.h>

#include <algorithm>
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

// --- the plain form (ttt2-xexinfo --write-plain, owner pipeline step 5c) -------------------------

namespace {

/// Number of header bytes that differ between two files, excluding the encryption/compression fields.
std::size_t header_differences(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                               const Xex2Image& parsed) {
  const auto* h = parsed.find_header(header_key::kFileFormatInfo);
  REQUIRE(h != nullptr);
  std::size_t n = 0;
  for (std::size_t i = 0; i < parsed.header_size; ++i) {
    if (i >= h->data + 4 && i < h->data + 8) continue;
    if (a[i] != b[i]) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("make_plain_xex turns the retail form into the plain one: headers kept, image decrypted") {
  const Fixture f = make_encrypted_fixture(true);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->file_format->encryption == EncryptionType::Normal);
  REQUIRE(parsed->file_format->compression == CompressionType::Basic);

  const auto plain = make_plain_xex(*parsed, f.file);
  if (!plain) FAIL(plain.error().message);
  CHECK(plain->size() == parsed->header_size + f.image.size());

  const auto reparsed = parse(*plain);
  REQUIRE(reparsed.has_value());
  REQUIRE(reparsed->file_format.has_value());
  CHECK(reparsed->file_format->encryption == EncryptionType::None);
  CHECK(reparsed->file_format->compression == CompressionType::None);
  CHECK(reparsed->header_size == parsed->header_size);
  CHECK(reparsed->security_offset == parsed->security_offset);
  CHECK(reparsed->optional_headers.size() == parsed->optional_headers.size());
  REQUIRE(reparsed->security.has_value());
  CHECK(reparsed->security->image_size == parsed->security->image_size);
  CHECK(reparsed->security->load_address == parsed->security->load_address);
  CHECK(reparsed->security->file_key == parsed->security->file_key);  // metadata stays, as XenonUtils keeps it
  CHECK(reparsed->security->header_digest == parsed->security->header_digest);
  REQUIRE(reparsed->execution_info.has_value());
  CHECK(reparsed->execution_info->title_id == 0x4E4D0859u);
  CHECK(reparsed->execution_info->version == parsed->execution_info->version);
  CHECK(reparsed->entry_point == parsed->entry_point);
  CHECK(header_differences(f.file, *plain, *parsed) == 0);

  // The image after the headers is the original PE image, and load_basefile agrees.
  CHECK(std::equal(f.image.begin(), f.image.end(), plain->begin() + parsed->header_size));
  const auto loaded = load_basefile(*reparsed, *plain);
  REQUIRE(loaded.has_value());
  CHECK(*loaded == f.image);
  CHECK(looks_like_pe_image(*loaded));

  const auto report = verify_plain_xex(*plain, *parsed, f.file);
  if (!report) FAIL(report.error().message);
  CHECK(report->header_size == parsed->header_size);
  CHECK(report->image_size == f.image.size());
  CHECK(hash::to_hex(report->image_sha1) == hash::to_hex(hash::Sha1::compute(f.image)));
}

TEST_CASE("make_plain_xex is idempotent and handles the unencrypted basic-compressed form") {
  SUBCASE("plain in, identical plain out") {
    const Fixture f = make_encrypted_fixture(true);
    const auto parsed = parse(f.file);
    REQUIRE(parsed.has_value());
    const auto plain = make_plain_xex(*parsed, f.file);
    REQUIRE(plain.has_value());
    const auto reparsed = parse(*plain);
    REQUIRE(reparsed.has_value());
    const auto again = make_plain_xex(*reparsed, *plain);
    REQUIRE(again.has_value());
    CHECK(*again == *plain);
  }
  SUBCASE("basic compression without encryption") {
    const Fixture f = make_encrypted_fixture(false);
    const auto parsed = parse(f.file);
    REQUIRE(parsed.has_value());
    const auto plain = make_plain_xex(*parsed, f.file);
    REQUIRE(plain.has_value());
    CHECK(std::equal(f.image.begin(), f.image.end(), plain->begin() + parsed->header_size));
    CHECK(verify_plain_xex(*plain, *parsed, f.file).has_value());
  }
}

TEST_CASE("make_plain_xex refuses what it cannot produce") {
  Fixture f = make_encrypted_fixture(true);
  auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  SUBCASE("wrong master key: not a PE image, nothing is produced") {
    BasefileOptions options;
    options.master_key = kDevkitKey;
    const auto r = make_plain_xex(*parsed, f.file, options);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("not a PE image") != std::string::npos);
  }
  SUBCASE("normal (LZX) compression") {
    parsed->file_format->compression = CompressionType::Normal;
    const auto r = make_plain_xex(*parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("LZX") != std::string::npos);
  }
  SUBCASE("delta patch") {
    parsed->file_format->compression = CompressionType::Delta;
    const auto r = make_plain_xex(*parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("delta patch") != std::string::npos);
  }
  SUBCASE("truncated input") {
    f.file.resize(parsed->header_size + 0x100);
    CHECK_FALSE(make_plain_xex(*parsed, f.file).has_value());
  }
}

TEST_CASE("verify_plain_xex catches a tampered or stale plain file") {
  const Fixture f = make_encrypted_fixture(true);
  const auto parsed = parse(f.file);
  REQUIRE(parsed.has_value());
  const auto plain = make_plain_xex(*parsed, f.file);
  REQUIRE(plain.has_value());
  const auto* h = parsed->find_header(header_key::kFileFormatInfo);
  REQUIRE(h != nullptr);

  SUBCASE("a changed image byte") {
    auto bad = *plain;
    bad[parsed->header_size + 0x2100] ^= 0x01;
    const auto r = verify_plain_xex(bad, *parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("image after the headers") != std::string::npos);
  }
  SUBCASE("a changed header byte") {
    auto bad = *plain;
    bad[0x18 + 4] ^= 0x01;  // the first optional header's value
    const auto r = verify_plain_xex(bad, *parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("header byte") != std::string::npos);
  }
  SUBCASE("compression field left as it was") {
    auto bad = *plain;
    store_be16(bad.data() + h->data + 6, 1);
    const auto r = verify_plain_xex(bad, *parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("still says") != std::string::npos);
  }
  SUBCASE("truncated output") {
    auto bad = *plain;
    bad.resize(bad.size() - 16);
    const auto r = verify_plain_xex(bad, *parsed, f.file);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("bytes; expected") != std::string::npos);
  }
  SUBCASE("the encrypted original is not its own plain form") {
    CHECK_FALSE(verify_plain_xex(f.file, *parsed, f.file).has_value());
  }
}

TEST_CASE("XexBuilder::basic_compress produces blocks decompress_basic inverts") {
  std::vector<std::uint8_t> image(0x5000, 0);
  for (std::size_t i = 0; i < 0x1000; ++i) image[i] = static_cast<std::uint8_t>(i + 1);          // page 0: data
  for (std::size_t i = 0x3000; i < 0x3800; ++i) image[i] = static_cast<std::uint8_t>(i * 3 + 1);  // page 3: data
  // pages 1, 2 and 4 are zero
  const auto c = XexBuilder::basic_compress(image);
  CHECK(c.data.size() == 0x2000);
  REQUIRE(c.blocks.size() == 2);
  CHECK(c.blocks[0] == std::pair<std::uint32_t, std::uint32_t>{0x1000, 0x2000});
  CHECK(c.blocks[1] == std::pair<std::uint32_t, std::uint32_t>{0x1000, 0x1000});
  std::vector<BasicCompressionBlock> blocks;
  for (const auto& [d, z] : c.blocks) blocks.push_back({d, z});
  const auto back = decompress_basic(c.data, blocks, static_cast<std::uint32_t>(image.size()));
  REQUIRE(back.has_value());
  CHECK(*back == image);

  // An image that starts with zero pages yields a first block without data.
  std::vector<std::uint8_t> leading(0x3000, 0);
  leading[0x2000] = 7;
  const auto l = XexBuilder::basic_compress(leading);
  REQUIRE(l.blocks.size() == 2);
  CHECK(l.blocks[0] == std::pair<std::uint32_t, std::uint32_t>{0, 0x2000});
  CHECK(l.blocks[1] == std::pair<std::uint32_t, std::uint32_t>{0x1000, 0});
}
