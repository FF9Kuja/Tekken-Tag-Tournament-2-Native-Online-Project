#include <doctest.h>

#include <string>
#include <vector>

#include "ttt2/core/hash.hpp"

using namespace ttt2::hash;

namespace {
std::string sha1_hex(std::string_view s) {
  return to_hex(Sha1::compute(s));
}
std::string sha256_hex(std::string_view s) {
  return to_hex(Sha256::compute(s));
}
}  // namespace

// Test vectors: FIPS 180-4 examples and the NIST "million a" vector.
TEST_CASE("SHA-1 known answers") {
  CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  const std::string million(1000000, 'a');
  CHECK(sha1_hex(million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("SHA-256 known answers") {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("incremental updates match one-shot hashing") {
  const std::string text = "The quick brown fox jumps over the lazy dog";
  Sha256 h;
  for (std::size_t i = 0; i < text.size(); ++i) h.update(text.data() + i, 1);  // byte at a time
  CHECK(to_hex(h.finish()) == sha256_hex(text));
  Sha1 h1;
  h1.update(text.substr(0, 17));
  h1.update(text.substr(17));
  CHECK(to_hex(h1.finish()) == sha1_hex(text));
  CHECK(sha1_hex(text) == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("hex conversion") {
  const std::vector<std::uint8_t> bytes = {0x00, 0xAB, 0xff, 0x10};
  CHECK(to_hex(bytes) == "00abff10");
  std::uint8_t out[4] = {};
  CHECK(from_hex("00ABff10", out));
  CHECK(out[1] == 0xAB);
  CHECK(out[2] == 0xFF);
  CHECK_FALSE(from_hex("00abff1", out));     // odd length
  CHECK_FALSE(from_hex("00abffzz", out));    // bad digit
  CHECK_FALSE(from_hex("00abff1000", out));  // too long for the buffer
}

TEST_CASE("FNV-1a 64") {
  const std::vector<std::uint8_t> empty;
  CHECK(fnv1a64(empty) == 0xcbf29ce484222325ull);
  const std::vector<std::uint8_t> a = {'a'};
  CHECK(fnv1a64(a) == 0xaf63dc4c8601ec8cull);
  const std::vector<std::uint8_t> foobar = {'f', 'o', 'o', 'b', 'a', 'r'};
  CHECK(fnv1a64(foobar) == 0x85944171f73967e8ull);
}
