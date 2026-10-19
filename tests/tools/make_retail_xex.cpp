// make_retail_xex: wraps a plain XEX2 executable (encryption none, compression none, such as the
// output of scripts/ppc_elf_to_xex.py) into the retail container form the owner's disc executable
// has: the PE image basic-compressed (zero pages left out) and AES-128-CBC encrypted with a session
// key that the security info stores encrypted with the public retail key. Every other header is
// copied as it is. No game data is involved: this exists so the SDK workflows can run the whole
// pipeline (ttt2-xexinfo --write-plain -> switch-table finder -> rexglue codegen -> host build ->
// boot) on a license-free program that arrives in the same form as the real executable.
//
//   make_retail_xex <plain.xex> <retail.xex> [--granule <bytes>]
//
// --granule sets the zero-run granularity of the basic compression (default 4096, a page: what retail
// executables use; a smaller value exercises the block table on small test images).
// Exit code 0 when the output was written and round-trip verified: it parses as encryption normal /
// compression basic, decrypts back to the input's image byte for byte, and make_plain_xex() turns it
// into a file whose image is that same image. 1 otherwise (nothing usable is left behind).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xex_builder.hpp"

#include "ttt2/core/aes.hpp"
#include "ttt2/core/endian.hpp"
#include "ttt2/core/file_io.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

namespace fs = std::filesystem;
using ttt2::test::XexBuilder;

int fail(const std::string& message) {
  std::fprintf(stderr, "make_retail_xex: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t granule = 0x1000;
  if (argc == 5 && std::string(argv[3]) == "--granule") {
    granule = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 0));
    if (granule < 16 || granule % 16 != 0) return fail("--granule must be a multiple of 16");
  } else if (argc != 3) {
    std::fprintf(stderr, "usage: make_retail_xex <plain.xex> <retail.xex> [--granule <bytes>]\n");
    return 1;
  }
  const fs::path in_path = argv[1];
  const fs::path out_path = argv[2];
  std::error_code ec;
  if (fs::exists(out_path, ec) && fs::equivalent(in_path, out_path, ec)) return fail("input and output are the same file");

  const auto file = ttt2::read_file(in_path);
  if (!file) return fail(file.error().message);
  const auto parsed = ttt2::xex::parse(*file);
  if (!parsed) return fail("input does not parse: " + parsed.error().message);
  if (!parsed->file_format || !parsed->security) return fail("input lacks file format or security info");
  if (parsed->file_format->encryption != ttt2::xex::EncryptionType::None ||
      parsed->file_format->compression != ttt2::xex::CompressionType::None) {
    return fail("input is not plain (encryption none, compression none); nothing to wrap");
  }
  const auto image = ttt2::xex::load_basefile(*parsed, *file);
  if (!image) return fail("input image: " + image.error().message);
  if (!ttt2::xex::looks_like_pe_image(*image)) return fail("input image is not a PE image");

  // Compress and encrypt like a retail executable. The session key is fixed (this is test data).
  auto compressed = XexBuilder::basic_compress(*image, granule);
  ttt2::aes::Key session{};
  for (std::size_t i = 0; i < session.size(); ++i) session[i] = static_cast<std::uint8_t>(0xC3 ^ (i * 29));
  ttt2::aes::Key stored_key{};
  ttt2::aes::Aes128(ttt2::xex::kRetailKey).encrypt_block(session.data(), stored_key.data());
  ttt2::aes::Aes128(session).cbc_encrypt(compressed.data, ttt2::aes::Block{});

  // Rebuild the container: every optional header as it was (FILE_FORMAT_INFO replaced), the security
  // info as it was with the stored session key, the encrypted compressed stream as the basefile.
  XexBuilder b;
  b.set_module_flags(parsed->module_flags);
  for (const auto& h : parsed->optional_headers) {
    if (h.is_inline()) {
      b.add_inline(h.key, h.data);
      continue;
    }
    if (h.key == ttt2::xex::header_key::kFileFormatInfo) {
      b.add_sized(h.key, XexBuilder::file_format_basic(1, compressed.blocks));
      continue;
    }
    const std::size_t offset = h.data;
    const std::size_t size = h.is_size_prefixed() ? ttt2::load_be32(file->data() + offset) : (h.key & 0xFF) * 4u;
    if (offset < 0x18 || offset + size > parsed->header_size) return fail("optional header outside the headers");
    b.add_struct(h.key, std::vector<std::uint8_t>(file->data() + offset, file->data() + offset + size));
  }
  const std::uint32_t sec_size = parsed->security->header_size;
  if (sec_size < 0x184 || static_cast<std::uint64_t>(parsed->security_offset) + sec_size > parsed->header_size) {
    return fail("security info outside the headers");
  }
  const std::size_t sec_offset = parsed->security_offset;
  std::vector<std::uint8_t> security(file->data() + sec_offset, file->data() + sec_offset + sec_size);
  std::copy(stored_key.begin(), stored_key.end(), security.begin() + 0x150);
  b.set_security(std::move(security));
  b.set_basefile(std::move(compressed.data));
  const auto out = b.build();

  // Round trip before anything is written.
  const auto reparsed = ttt2::xex::parse(out);
  if (!reparsed) return fail("output does not parse: " + reparsed.error().message);
  if (!reparsed->file_format || reparsed->file_format->encryption != ttt2::xex::EncryptionType::Normal ||
      reparsed->file_format->compression != ttt2::xex::CompressionType::Basic) {
    return fail("output is not in the retail form");
  }
  const auto recovered = ttt2::xex::load_basefile(*reparsed, out);
  if (!recovered || *recovered != *image) return fail("output does not decrypt back to the input's image");
  const auto plain = ttt2::xex::make_plain_xex(*reparsed, out);
  if (!plain) return fail("make_plain_xex on the output: " + plain.error().message);
  const auto plain_image = ttt2::xex::load_basefile(*ttt2::xex::parse(*plain), *plain);
  if (!plain_image || *plain_image != *image) return fail("the plain form of the output has a different image");
  if (ttt2::xex::derive_session_key(*reparsed->security) != session) return fail("session key does not round-trip");

  if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path(), ec);
  std::ofstream stream(out_path, std::ios::binary | std::ios::trunc);
  if (!stream || !stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
    fs::remove(out_path, ec);
    return fail("cannot write " + out_path.string());
  }
  std::printf("wrote %s (%zu bytes): encryption normal, compression basic (%zu blocks), image %zu bytes, "
              "image SHA-1 %s; round trip verified\n",
              out_path.string().c_str(), out.size(), reparsed->file_format->basic_blocks.size(), image->size(),
              ttt2::hash::to_hex(ttt2::hash::Sha1::compute(*image)).c_str());
  return 0;
}
