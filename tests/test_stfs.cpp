#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "stfs_builder.hpp"

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/stfs/stfs.hpp"

using namespace ttt2;
using namespace ttt2::stfs;
using ttt2::test::StfsBuilder;

namespace {
std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31 + (i >> 8));
  return v;
}
}  // namespace

TEST_CASE("block arithmetic skips the hash tables") {
  CHECK(backing_block(0, 0) == 0);
  CHECK(backing_block(0xA9, 0) == 0xA9);
  CHECK(backing_block(0xAA, 0) == 0xAA + 2);  // one table before run 0 (implicit) and one before run 1
  CHECK(backing_block(0x154, 0) == 0x154 + 3);
  CHECK(backing_block(0xAA, 1) == 0xAA + 4);  // two blocks per table
  CHECK(block_offset(0, 0) == 0xC000);
  CHECK(block_offset(1, 0) == 0xD000);
  CHECK(hash_entry_offset(0, 0) == 0xB000);
  CHECK(hash_entry_offset(5, 0) == 0xB000 + 5 * 0x18);
  CHECK(hash_entry_offset(0xAA, 0) == block_offset(0xAA, 0) - 0x1000);
  CHECK(hash_entry_offset(0xAB, 0) == block_offset(0xAA, 0) - 0x1000 + 0x18);
  CHECK(backing_block(0x70E4, 0) == 0x70E4 + (0x70E4 / 0xAA + 1) + 2);
}

TEST_CASE("reads a synthetic title update package") {
  StfsBuilder b;
  b.set_title(0x4E4D0859u, kContentTypeTitleUpdate, 0x00000302u);
  b.set_display_name("Title Update #3");
  const auto xexp = pattern(0x2345, 7);
  const auto bin = pattern(0xAA * 0x1000 + 123, 9);  // spans more than one hash-table run
  b.add_file("Default.xexp", xexp);
  b.add_file("data100.bin", bin);
  b.add_file("data100.ofs", pattern(16, 1));
  const auto image = b.build();
  MemorySource src(image);
  auto pkg = Package::open(src);
  REQUIRE(pkg.has_value());
  CHECK(pkg->info().magic == Magic::Live);
  CHECK(pkg->info().title_id == 0x4E4D0859u);
  CHECK(pkg->info().is_title_update());
  CHECK(pkg->info().table_shift == 0);
  CHECK(pkg->info().display_name == "Title Update #3");
  CHECK(pkg->info().file_table_block_count == 1);
  const auto entries = pkg->list();
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 3);
  CHECK((*entries)[0].name == "Default.xexp");
  CHECK((*entries)[0].size == 0x2345);
  CHECK((*entries)[1].allocated_blocks == 0xAB);
  CHECK(pkg->path_of(*entries, 1) == "data100.bin");

  const auto found = pkg->find("default.XEXP");
  REQUIRE(found.has_value());
  const auto data = pkg->read_file(*found, 1 << 20);
  REQUIRE(data.has_value());
  CHECK(*data == xexp);

  const auto big = pkg->find("data100.bin");
  REQUIRE(big.has_value());
  hash::Sha1 h;
  const auto n = pkg->stream_file(*big, [&](std::span<const std::uint8_t> c) {
    h.update(c);
    return true;
  });
  REQUIRE(n.has_value());
  CHECK(*n == bin.size());
  CHECK(hash::to_hex(h.finish()) == hash::to_hex(hash::Sha1::compute(bin)));
  CHECK_FALSE(pkg->find("missing").has_value());
  CHECK_FALSE(pkg->read_file(*big, 100).has_value());
}

TEST_CASE("non-sequential block chains are followed through the hash entries") {
  StfsBuilder b;
  b.set_scramble(true);
  const auto a = pattern(0x3000, 3);
  const auto c = pattern(0x2800, 5);
  b.add_file("a.bin", a);
  b.add_file("c.bin", c);
  const auto image = b.build();
  MemorySource src(image);
  auto pkg = Package::open(src);
  REQUIRE(pkg.has_value());
  const auto ea = pkg->find("a.bin");
  const auto ec = pkg->find("c.bin");
  REQUIRE(ea.has_value());
  REQUIRE(ec.has_value());
  CHECK(pkg->read_file(*ea, 1 << 20).value() == a);
  CHECK(pkg->read_file(*ec, 1 << 20).value() == c);
  // The chain really is non-sequential.
  const auto next = pkg->next_block(ea->start_block);
  REQUIRE(next.has_value());
  CHECK(*next != ea->start_block + 1);
}

TEST_CASE("consecutive files do not need the chain") {
  StfsBuilder b;
  const auto a = pattern(0x5000, 11);
  b.add_file("seq.bin", a, true);
  const auto image = b.build();
  MemorySource src(image);
  auto pkg = Package::open(src);
  REQUIRE(pkg.has_value());
  const auto e = pkg->find("seq.bin");
  REQUIRE(e.has_value());
  CHECK(e->consecutive);
  CHECK(pkg->read_file(*e, 1 << 20).value() == a);
}

TEST_CASE("rejects damaged packages without crashing") {
  StfsBuilder b;
  b.add_file("x.bin", pattern(0x1800, 2));
  const auto pristine = b.build();
  SUBCASE("bad magic") {
    auto image = pristine;
    image[0] = 'X';
    MemorySource s(image);
    CHECK_FALSE(Package::open(s).has_value());
  }
  SUBCASE("too small") {
    std::vector<std::uint8_t> tiny(0x400, 0);
    MemorySource s(tiny);
    CHECK_FALSE(Package::open(s).has_value());
  }
  SUBCASE("file chain cut short") {
    auto image = pristine;
    // end-of-chain after the first block although the file needs two
    const std::size_t e = 0xB000 + 1 * 0x18;  // hash entry of block 1 (x.bin starts at block 1)
    image[e + 0x15] = 0xFF;
    image[e + 0x16] = 0xFF;
    image[e + 0x17] = 0xFF;
    MemorySource s(image);
    auto pkg = Package::open(s);
    REQUIRE(pkg.has_value());
    const auto x = pkg->find("x.bin");
    REQUIRE(x.has_value());
    CHECK_FALSE(pkg->read_file(*x, 1 << 20).has_value());
  }
  SUBCASE("file chain loops") {
    auto image = pristine;
    const std::size_t e = 0xB000 + 1 * 0x18;
    image[e + 0x15] = 0;
    image[e + 0x16] = 0;
    image[e + 0x17] = 1;  // block 1 -> block 1
    MemorySource s(image);
    auto pkg = Package::open(s);
    REQUIRE(pkg.has_value());
    const auto x = pkg->find("x.bin");
    REQUIRE(x.has_value());
    CHECK_FALSE(pkg->read_file(*x, 1 << 20).has_value());
  }
  SUBCASE("truncated file") {
    auto image = pristine;
    image.resize(0xD800);
    MemorySource s(image);
    auto pkg = Package::open(s);
    REQUIRE(pkg.has_value());
    const auto x = pkg->find("x.bin");
    REQUIRE(x.has_value());
    CHECK_FALSE(pkg->read_file(*x, 1 << 20).has_value());
  }
}
