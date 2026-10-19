#include "ttt2/xex/basefile.hpp"

#include <algorithm>
#include <cstring>

#include "ttt2/core/byte_reader.hpp"

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

}  // namespace ttt2::xex
