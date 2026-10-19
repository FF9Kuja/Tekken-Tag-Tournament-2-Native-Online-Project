#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "xdvdfs_builder.hpp"
#include "xex_builder.hpp"

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/xdvdfs/xdvdfs.hpp"
#include "ttt2/xex/xex2.hpp"

using namespace ttt2;
using namespace ttt2::xdvdfs;
using ttt2::test::XdvdfsBuilder;
using ttt2::test::XexBuilder;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::vector<std::uint8_t> synthetic_xex() {
  XexBuilder b;
  b.add_inline(xex::header_key::kEntryPoint, 0x82110000u);
  b.add_struct(xex::header_key::kExecutionInfo,
               XexBuilder::execution_info(0x12345678u, 0x00000002u, 0x00000002u, 0x4E4D0859u, 1, 1, 0));
  b.set_security(XexBuilder::security_info(0x1000, 0, 0x82000000u, 0xFFFFFFFFu, 0, {}));
  b.set_basefile(std::vector<std::uint8_t>(64, 0xEE));
  return b.build();
}

std::vector<std::uint8_t> make_image(std::uint64_t partition_offset) {
  XdvdfsBuilder b(partition_offset);
  b.add_file("default.xex", synthetic_xex());
  b.add_file("nrfo.bin", bytes_of("not a real container"));
  b.add_file("DATA00.BIN", std::vector<std::uint8_t>(5000, 0x5A));  // crosses sector boundaries
  b.add_file("movie/intro.wmv", bytes_of("movie bytes"));
  b.add_file("movie/sub/deep.txt", bytes_of("deep"));
  b.add_file("sound/bgm.xma", bytes_of("xma"));
  b.add_file("Zlast.dat", bytes_of("z"));
  b.add_file("alpha.dat", bytes_of("a"));
  return b.build();
}

}  // namespace

TEST_CASE("opens images at every known partition offset") {
  for (const std::uint64_t base : {0ull, 0x2080000ull}) {
    const auto image = make_image(base);
    MemorySource src(image);
    const auto disc = Image::open(src);
    REQUIRE(disc.has_value());
    CHECK(disc->volume().partition_offset == base);
    CHECK(disc->volume().root_size > 0);
  }
}

TEST_CASE("rejects non-XDVDFS data") {
  std::vector<std::uint8_t> junk(0x30000, 0x41);
  MemorySource src(junk);
  CHECK_FALSE(Image::open(src).has_value());
  std::vector<std::uint8_t> tiny(100, 0);
  MemorySource tiny_src(tiny);
  CHECK_FALSE(Image::open(tiny_src).has_value());
}

TEST_CASE("lists the root in name order and descends into directories") {
  const auto image = make_image(0);
  MemorySource src(image);
  auto disc = Image::open(src);
  REQUIRE(disc.has_value());
  const auto root = disc->list("/");
  REQUIRE(root.has_value());
  std::vector<std::string> names;
  for (const auto& e : *root) names.push_back(e.name);
  CHECK(names == std::vector<std::string>{"alpha.dat", "DATA00.BIN", "default.xex", "movie", "nrfo.bin",
                                          "sound", "Zlast.dat"});
  const auto movie = disc->list("movie");
  REQUIRE(movie.has_value());
  REQUIRE(movie->size() == 2);
  CHECK((*movie)[0].name == "intro.wmv");
  CHECK((*movie)[1].name == "sub");
  CHECK((*movie)[1].is_directory());
  const auto deep = disc->list("MOVIE\\SUB");
  REQUIRE(deep.has_value());
  REQUIRE(deep->size() == 1);
  CHECK((*deep)[0].name == "deep.txt");
  CHECK_FALSE(disc->list("nope").has_value());
  CHECK_FALSE(disc->list("default.xex").has_value());  // a file, not a directory
}

TEST_CASE("finds files case-insensitively and reads them back") {
  const auto image = make_image(0x2080000ull);
  MemorySource src(image);
  auto disc = Image::open(src);
  REQUIRE(disc.has_value());
  const auto xex = disc->find("DEFAULT.XEX");
  REQUIRE(xex.has_value());
  CHECK_FALSE(xex->is_directory());
  const auto data = disc->read_file(*xex, 1 << 20);
  REQUIRE(data.has_value());
  CHECK(*data == synthetic_xex());
  const auto parsed = xex::parse(*data);
  REQUIRE(parsed.has_value());
  CHECK(parsed->execution_info->title_id == 0x4E4D0859u);

  const auto deep = disc->find("movie/sub/DEEP.txt");
  REQUIRE(deep.has_value());
  CHECK(disc->read_file(*deep, 100).value() == bytes_of("deep"));

  const auto big = disc->find("data00.bin");
  REQUIRE(big.has_value());
  CHECK(big->size == 5000);
  CHECK_FALSE(disc->read_file(*big, 100).has_value());  // above the caller's limit
  std::uint64_t streamed = 0;
  hash::Sha1 h;
  const auto n = disc->stream_file(
      *big,
      [&](std::span<const std::uint8_t> chunk) {
        streamed += chunk.size();
        h.update(chunk);
        return true;
      },
      1024);
  REQUIRE(n.has_value());
  CHECK(*n == 5000);
  CHECK(streamed == 5000);
  CHECK(hash::to_hex(h.finish()) == hash::to_hex(hash::Sha1::compute(std::vector<std::uint8_t>(5000, 0x5A))));
  CHECK_FALSE(disc->find("movie/missing").has_value());
  CHECK_FALSE(disc->find("").has_value());
}

TEST_CASE("corrupt directory tables are rejected without crashing") {
  const auto pristine = make_image(0);
  MemorySource src(pristine);
  auto disc = Image::open(src);
  REQUIRE(disc.has_value());
  const std::size_t root_off = static_cast<std::size_t>(disc->volume().root_sector) * kSectorSize;

  SUBCASE("a name that runs past the table") {
    auto image = pristine;
    // Shrink the root table to 16 bytes in the volume descriptor: the first
    // entry's 14-byte header fits, its name does not.
    const std::size_t descriptor = static_cast<std::size_t>(kVolumeDescriptorSector) * kSectorSize;
    image[descriptor + 24] = 16;
    image[descriptor + 25] = 0;
    image[descriptor + 26] = 0;
    image[descriptor + 27] = 0;
    MemorySource s2(image);
    auto d2 = Image::open(s2);
    REQUIRE(d2.has_value());
    const auto r = d2->list("/");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("truncated") != std::string::npos);
  }
  SUBCASE("a link cycle") {
    auto image = pristine;
    // The root's right child: make its own right link point back at itself.
    const std::uint16_t right = static_cast<std::uint16_t>(image[root_off + 2] | (image[root_off + 3] << 8));
    REQUIRE(right != 0);
    const std::size_t child = root_off + static_cast<std::size_t>(right) * 4;
    image[child + 2] = static_cast<std::uint8_t>(right);
    image[child + 3] = static_cast<std::uint8_t>(right >> 8);
    MemorySource s2(image);
    auto d2 = Image::open(s2);
    REQUIRE(d2.has_value());
    const auto r = d2->list("/");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("cycle") != std::string::npos);
  }
  SUBCASE("a link outside the table") {
    auto image = pristine;
    image[root_off + 2] = 0xF0;
    image[root_off + 3] = 0x3F;  // dword 0x3FF0 = byte 0xFFC0, past a one-sector table
    MemorySource s2(image);
    auto d2 = Image::open(s2);
    REQUIRE(d2.has_value());
    CHECK_FALSE(d2->list("/").has_value());
  }
  SUBCASE("a file beyond the end of the image") {
    auto image = pristine;
    const auto entry = disc->find("alpha.dat");
    REQUIRE(entry.has_value());
    Entry far = *entry;
    far.start_sector = 0x7FFFFFFFu;
    CHECK_FALSE(disc->read_file(far, 100).has_value());
    CHECK_FALSE(disc->stream_file(far, [](std::span<const std::uint8_t>) { return true; }).has_value());
  }
}
