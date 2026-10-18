// SHA-1 (FIPS 180-4). Straightforward reference implementation; speed is not
// a concern for the file sizes this project hashes.

#include <cstring>

#include "ttt2/core/endian.hpp"
#include "ttt2/core/hash.hpp"

namespace ttt2::hash {

namespace {

constexpr std::uint32_t rotl(std::uint32_t v, unsigned n) noexcept {
  return (v << n) | (v >> (32u - n));
}

}  // namespace

Sha1::Sha1() noexcept
    : state_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u},
      total_bytes_(0),
      buffer_{},
      buffered_(0) {}

void Sha1::process_block(const std::uint8_t* block) noexcept {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f;
    std::uint32_t k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
}

void Sha1::update(const void* data, std::size_t length) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_bytes_ += length;
  if (buffered_ > 0) {
    const std::size_t take = (64 - buffered_) < length ? (64 - buffered_) : length;
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    length -= take;
    if (buffered_ == 64) {
      process_block(buffer_);
      buffered_ = 0;
    }
  }
  while (length >= 64) {
    process_block(p);
    p += 64;
    length -= 64;
  }
  if (length > 0) {
    std::memcpy(buffer_, p, length);
    buffered_ = length;
  }
}

Sha1::Digest Sha1::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8;
  const std::uint8_t pad = 0x80;
  update(&pad, 1);
  const std::uint8_t zero = 0;
  while (buffered_ != 56) update(&zero, 1);
  std::uint8_t len[8];
  store_be64(len, bit_length);
  update(len, 8);  // completes the final block
  Digest digest;
  for (int i = 0; i < 5; ++i) store_be32(digest.data() + i * 4, state_[i]);
  return digest;
}

Sha1::Digest Sha1::compute(std::span<const std::uint8_t> data) noexcept {
  Sha1 h;
  h.update(data);
  return h.finish();
}

Sha1::Digest Sha1::compute(std::string_view text) noexcept {
  Sha1 h;
  h.update(text);
  return h.finish();
}

}  // namespace ttt2::hash
