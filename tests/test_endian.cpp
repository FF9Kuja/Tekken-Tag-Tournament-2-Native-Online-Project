#include <doctest.h>

#include <cstdint>
#include <vector>

#include "ttt2/core/byte_reader.hpp"
#include "ttt2/core/endian.hpp"

using namespace ttt2;

TEST_CASE("byteswap reverses byte order") {
  CHECK(byteswap16(0x1234) == 0x3412);
  CHECK(byteswap32(0x12345678u) == 0x78563412u);
  CHECK(byteswap64(0x0102030405060708ull) == 0x0807060504030201ull);
  CHECK(byteswap<std::int32_t>(0x11223344) == 0x44332211);
  CHECK(byteswap<std::uint8_t>(0xAB) == 0xAB);
  static_assert(byteswap32(0xDEADBEEFu) == 0xEFBEADDEu);
}

TEST_CASE("host_to_be / be_to_host round trip") {
  const std::uint32_t v = 0xCAFEBABEu;
  CHECK(be_to_host(host_to_be(v)) == v);
  const be<std::uint32_t> stored(v);
  CHECK(stored.get() == v);
  CHECK(static_cast<std::uint32_t>(stored) == v);
  // The in-memory representation must be big-endian on every host.
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&stored);
  CHECK(bytes[0] == 0xCA);
  CHECK(bytes[1] == 0xFE);
  CHECK(bytes[2] == 0xBA);
  CHECK(bytes[3] == 0xBE);
}

TEST_CASE("unaligned big-endian loads and stores") {
  std::uint8_t buf[9] = {0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  CHECK(load_be16(buf + 1) == 0x0102);
  CHECK(load_be32(buf + 1) == 0x01020304u);
  CHECK(load_be64(buf + 1) == 0x0102030405060708ull);
  store_be32(buf + 1, 0xA1B2C3D4u);
  CHECK(buf[1] == 0xA1);
  CHECK(buf[4] == 0xD4);
  CHECK(load_be32(buf + 1) == 0xA1B2C3D4u);
  store_be16(buf + 5, 0xBEEF);
  CHECK(buf[5] == 0xBE);
  CHECK(buf[6] == 0xEF);
  store_be64(buf + 1, 0x1122334455667788ull);
  CHECK(load_be64(buf + 1) == 0x1122334455667788ull);
}

TEST_CASE("ByteReader never reads out of bounds") {
  const std::vector<std::uint8_t> data = {0x00, 0x01, 0x02, 0x03, 'a', 'b', 0, 'c'};
  const ByteReader r(data);
  CHECK(r.size() == 8);
  CHECK(r.u32be(0) == 0x00010203u);
  CHECK(r.u16be(2) == 0x0203);
  CHECK(r.u8(7) == 'c');
  CHECK_FALSE(r.u8(8).has_value());
  CHECK_FALSE(r.u32be(5).has_value());
  CHECK_FALSE(r.u64be(1).has_value());
  CHECK(r.u64be(0).has_value());
  CHECK(r.in_bounds(8, 0));
  CHECK_FALSE(r.in_bounds(9, 0));
  CHECK_FALSE(r.in_bounds(4, 5));
  // Overflow-safe: a huge length must not wrap around.
  CHECK_FALSE(r.in_bounds(1, static_cast<std::size_t>(-1)));
  CHECK(r.cstring(4, 10) == std::string("ab"));
  CHECK(r.cstring(4, 1) == std::string("a"));
  CHECK(r.cstring(7, 100) == std::string("c"));  // no NUL inside the window: all bytes returned
  CHECK_FALSE(r.cstring(9, 4).has_value());
  const auto sub = r.bytes(4, 4);
  REQUIRE(sub.has_value());
  CHECK(sub->size() == 4);
  CHECK_FALSE(r.bytes(6, 3).has_value());
}
