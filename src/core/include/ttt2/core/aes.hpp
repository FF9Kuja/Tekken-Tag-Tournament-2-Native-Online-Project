// AES-128 (FIPS-197), ECB single-block and CBC multi-block operations.
//
// Xbox 360 executables are encrypted with AES-128-CBC: the per-file key in
// the XEX security info is itself encrypted (ECB) with a publicly known
// retail key, and the basefile is CBC-encrypted with the resulting session
// key and a zero IV. This is a small, dependency-free implementation written
// from the specification and checked against the FIPS-197 / SP 800-38A test
// vectors (tests/test_aes.cpp). Not constant-time; it never handles secrets
// that matter (the keys involved are public or derived from the file).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ttt2::aes {

inline constexpr std::size_t kBlockSize = 16;
inline constexpr std::size_t kKeySize = 16;

using Block = std::array<std::uint8_t, kBlockSize>;
using Key = std::array<std::uint8_t, kKeySize>;

class Aes128 {
 public:
  explicit Aes128(const Key& key) noexcept;

  void encrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept;
  void decrypt_block(const std::uint8_t* in, std::uint8_t* out) const noexcept;

  /// CBC decryption in place. `data.size()` must be a multiple of 16.
  void cbc_decrypt(std::span<std::uint8_t> data, const Block& iv) const noexcept;
  /// CBC encryption in place (used by tests to build encrypted fixtures).
  void cbc_encrypt(std::span<std::uint8_t> data, const Block& iv) const noexcept;

 private:
  std::array<std::uint8_t, 176> round_keys_{};  // 11 round keys of 16 bytes
};

}  // namespace ttt2::aes
