#include <doctest.h>

#include <array>
#include <vector>

#include "ttt2/core/aes.hpp"
#include "ttt2/core/hash.hpp"

using namespace ttt2;

namespace {
template <std::size_t N>
std::array<std::uint8_t, N> bytes(const char* hex) {
  std::array<std::uint8_t, N> out{};
  REQUIRE(hash::from_hex(hex, out));
  return out;
}
}  // namespace

TEST_CASE("AES-128 single block matches FIPS-197 appendix C.1") {
  const aes::Aes128 cipher(bytes<16>("000102030405060708090a0b0c0d0e0f"));
  const auto plain = bytes<16>("00112233445566778899aabbccddeeff");
  std::array<std::uint8_t, 16> out{};
  cipher.encrypt_block(plain.data(), out.data());
  CHECK(hash::to_hex(out) == "69c4e0d86a7b0430d8cdb78070b4c55a");
  std::array<std::uint8_t, 16> back{};
  cipher.decrypt_block(out.data(), back.data());
  CHECK(back == plain);
}

TEST_CASE("AES-128-CBC matches SP 800-38A F.2.1 / F.2.2") {
  const aes::Aes128 cipher(bytes<16>("2b7e151628aed2a6abf7158809cf4f3c"));
  const auto iv = bytes<16>("000102030405060708090a0b0c0d0e0f");
  const auto p1 = bytes<16>("6bc1bee22e409f96e93d7e117393172a");
  const auto p2 = bytes<16>("ae2d8a571e03ac9c9eb76fac45af8e51");
  std::vector<std::uint8_t> data(p1.begin(), p1.end());
  data.insert(data.end(), p2.begin(), p2.end());
  cipher.cbc_encrypt(data, iv);
  CHECK(hash::to_hex(std::span<const std::uint8_t>(data.data(), 16)) == "7649abac8119b246cee98e9b12e9197d");
  CHECK(hash::to_hex(std::span<const std::uint8_t>(data.data() + 16, 16)) ==
        "5086cb9b507219ee95db113a917678b2");
  cipher.cbc_decrypt(data, iv);
  CHECK(hash::to_hex(std::span<const std::uint8_t>(data.data(), 16)) == "6bc1bee22e409f96e93d7e117393172a");
  CHECK(hash::to_hex(std::span<const std::uint8_t>(data.data() + 16, 16)) ==
        "ae2d8a571e03ac9c9eb76fac45af8e51");
}

TEST_CASE("AES-128 round trip over many blocks with a zero IV") {
  const aes::Aes128 cipher(bytes<16>("20b185a59d28fdc340583fbb0896bf91"));
  std::vector<std::uint8_t> data(16 * 100);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7 + 3);
  const auto original = data;
  cipher.cbc_encrypt(data, aes::Block{});
  CHECK(data != original);
  cipher.cbc_decrypt(data, aes::Block{});
  CHECK(data == original);
}
