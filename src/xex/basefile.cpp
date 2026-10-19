#include "ttt2/xex/basefile.hpp"

#include <algorithm>
#include <cstring>

#include "ttt2/core/byte_reader.hpp"
#include "ttt2/core/endian.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"

namespace ttt2::xex {

aes::Key derive_session_key(const SecurityInfo& security, const aes::Key& master_key) noexcept {
  const aes::Aes128 master(master_key);
  aes::Key session{};
  master.decrypt_block(security.file_key.data(), session.data());
  return session;
}

Expected<std::vector<std::uint8_t>> decompress_basic(std::span<const std::uint8_t> compressed,
                                                     const std::vector<BasicCompressionBlock>& blocks,
                                                     std::uint32_t image_size) {
  std::vector<std::uint8_t> out(image_size, 0);
  std::size_t in_pos = 0;
  std::size_t out_pos = 0;
  for (const auto& b : blocks) {
    if (b.data_size > compressed.size() - in_pos) {
      return make_error("basic compression block reads past the end of the encrypted data");
    }
    if (static_cast<std::uint64_t>(b.data_size) + b.zero_size > out.size() - out_pos) {
      return make_error("basic compression blocks produce more than image_size bytes");
    }
    std::memcpy(out.data() + out_pos, compressed.data() + in_pos, b.data_size);
    in_pos += b.data_size;
    out_pos += static_cast<std::size_t>(b.data_size) + b.zero_size;
  }
  return out;
}

bool looks_like_pe_image(std::span<const std::uint8_t> image) noexcept {
  const ByteReader r(image);
  if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return false;
  // e_lfanew is little-endian in the DOS header even on this big-endian platform.
  const std::uint32_t e_lfanew =
      static_cast<std::uint32_t>(image[0x3C]) | (static_cast<std::uint32_t>(image[0x3D]) << 8) |
      (static_cast<std::uint32_t>(image[0x3E]) << 16) | (static_cast<std::uint32_t>(image[0x3F]) << 24);
  if (!r.in_bounds(e_lfanew, 4)) return false;
  return image[e_lfanew] == 'P' && image[e_lfanew + 1] == 'E' && image[e_lfanew + 2] == 0 &&
         image[e_lfanew + 3] == 0;
}

Expected<std::vector<std::uint8_t>> load_basefile(const Xex2Image& image, std::span<const std::uint8_t> file,
                                                  const BasefileOptions& options) {
  if (!image.security) return make_error("the XEX has no readable security info");
  if (!image.file_format) return make_error("the XEX has no file format info header");
  const auto& fmt = *image.file_format;
  const std::uint32_t image_size = image.security->image_size;
  if (image_size == 0 || image_size > options.max_image_size) {
    return make_error("implausible image size " + std::to_string(image_size));
  }
  if (image.header_size > file.size()) return make_error("the file is shorter than its header size");

  std::vector<std::uint8_t> data(file.begin() + image.header_size, file.end());

  switch (fmt.encryption) {
    case EncryptionType::None:
      break;
    case EncryptionType::Normal: {
      const aes::Aes128 session(derive_session_key(*image.security, options.master_key));
      // CBC works on whole blocks; a trailing partial block is not part of the image.
      const std::size_t whole = data.size() - (data.size() % aes::kBlockSize);
      session.cbc_decrypt(std::span<std::uint8_t>(data.data(), whole), aes::Block{});
      break;
    }
    default:
      return make_error("unsupported encryption type");
  }

  switch (fmt.compression) {
    case CompressionType::None:
      if (data.size() < image_size) return make_error("the uncompressed basefile is shorter than image_size");
      data.resize(image_size);
      return data;
    case CompressionType::Basic:
      return decompress_basic(data, fmt.basic_blocks, image_size);
    case CompressionType::Normal:
      return make_error("normal (LZX) compression is not implemented here; use the XenonUtils-based tool");
    case CompressionType::Delta:
      return make_error("this is a delta patch (.xexp); apply it to the base image with the patch tool");
  }
  return make_error("unknown compression type");
}

namespace {

/// File offset of the FILE_FORMAT_INFO structure (its size dword) inside the headers, or an error.
Expected<std::uint32_t> file_format_info_offset(const Xex2Image& image) {
  const OptionalHeader* h = image.find_header(header_key::kFileFormatInfo);
  if (h == nullptr || h->is_inline()) return make_error("the XEX has no FILE_FORMAT_INFO header");
  // size dword, encryption u16, compression u16: the eight bytes the plain form rewrites
  if (h->data < 0x18 || static_cast<std::uint64_t>(h->data) + 8 > image.header_size) {
    return make_error("the FILE_FORMAT_INFO header lies outside the XEX headers");
  }
  return h->data;
}

}  // namespace

Expected<std::vector<std::uint8_t>> make_plain_xex(const Xex2Image& image, std::span<const std::uint8_t> file,
                                                   const BasefileOptions& options) {
  const auto offset = file_format_info_offset(image);
  if (!offset) return offset.error();
  auto pe = load_basefile(image, file, options);
  if (!pe) return pe.error();
  if (!looks_like_pe_image(*pe)) {
    return make_error("the decrypted image is not a PE image (wrong key or damaged file); not writing it");
  }

  std::vector<std::uint8_t> out(file.begin(), file.begin() + image.header_size);
  store_be16(out.data() + *offset + 4, static_cast<std::uint16_t>(EncryptionType::None));
  store_be16(out.data() + *offset + 6, static_cast<std::uint16_t>(CompressionType::None));
  out.insert(out.end(), pe->begin(), pe->end());

  const auto check = verify_plain_xex(out, image, file, options);
  if (!check) return make_error("self-check of the plain image failed: " + check.error().message);
  return out;
}

Expected<PlainXexReport> verify_plain_xex(std::span<const std::uint8_t> plain, const Xex2Image& original,
                                          std::span<const std::uint8_t> original_file,
                                          const BasefileOptions& options) {
  const auto offset = file_format_info_offset(original);
  if (!offset) return offset.error();
  const auto expected = load_basefile(original, original_file, options);
  if (!expected) return make_error("the original's image cannot be produced: " + expected.error().message);

  const auto parsed = parse(plain);
  if (!parsed) return make_error("the plain file does not parse as XEX2: " + parsed.error().message);
  if (!parsed->file_format) return make_error("the plain file has no file format info header");
  if (parsed->file_format->encryption != EncryptionType::None ||
      parsed->file_format->compression != CompressionType::None) {
    return make_error("the plain file still says encryption " +
                      std::to_string(static_cast<unsigned>(parsed->file_format->encryption)) + ", compression " +
                      std::to_string(static_cast<unsigned>(parsed->file_format->compression)));
  }
  if (parsed->header_size != original.header_size) {
    return make_error("header size changed: 0x" + hex_u32(parsed->header_size) + " vs 0x" + hex_u32(original.header_size));
  }
  if (!parsed->security || !original.security || parsed->security->image_size != original.security->image_size ||
      parsed->security->load_address != original.security->load_address) {
    return make_error("the security info (image size, load address) does not match the original's");
  }
  const std::size_t header_size = original.header_size;
  if (plain.size() != header_size + expected->size()) {
    return make_error("the plain file is " + std::to_string(plain.size()) + " bytes; expected " +
                      std::to_string(header_size + expected->size()) + " (headers + image)");
  }
  if (original_file.size() < header_size) return make_error("the original is shorter than its header size");
  for (std::size_t i = 0; i < header_size; ++i) {
    if (i >= *offset + 4 && i < *offset + 8) continue;  // the encryption and compression fields
    if (plain[i] != original_file[i]) {
      return make_error("header byte 0x" + hex_u32(static_cast<std::uint32_t>(i)) + " differs from the original");
    }
  }
  if (!std::equal(expected->begin(), expected->end(), plain.begin() + static_cast<std::ptrdiff_t>(header_size))) {
    return make_error("the image after the headers is not the decrypted, decompressed image of the original");
  }
  PlainXexReport report;
  report.header_size = original.header_size;
  report.image_size = static_cast<std::uint32_t>(expected->size());
  report.image_sha1 = hash::Sha1::compute(*expected);
  return report;
}

}  // namespace ttt2::xex
