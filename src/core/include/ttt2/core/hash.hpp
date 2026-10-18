// Cryptographic and non-cryptographic hashes used for game-file validation
// (SHA-1 / SHA-256 of user-supplied executables and disc files) and, later,
// for fast state hashing in determinism and rollback diagnostics (FNV-1a).
//
// Implemented locally from the public specifications (FIPS 180-4) to keep the
// foundation dependency-free. Verified against the NIST test vectors in
// tests/test_hash.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ttt2::hash {

class Sha1 {
 public:
  static constexpr std::size_t kDigestSize = 20;
  using Digest = std::array<std::uint8_t, kDigestSize>;

  Sha1() noexcept;
  void update(const void* data, std::size_t length) noexcept;
  void update(std::span<const std::uint8_t> data) noexcept { update(data.data(), data.size()); }
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  [[nodiscard]] Digest finish() noexcept;

  [[nodiscard]] static Digest compute(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] static Digest compute(std::string_view text) noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::uint32_t state_[5];
  std::uint64_t total_bytes_;
  std::uint8_t buffer_[64];
  std::size_t buffered_;
};

class Sha256 {
 public:
  static constexpr std::size_t kDigestSize = 32;
  using Digest = std::array<std::uint8_t, kDigestSize>;

  Sha256() noexcept;
  void update(const void* data, std::size_t length) noexcept;
  void update(std::span<const std::uint8_t> data) noexcept { update(data.data(), data.size()); }
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  [[nodiscard]] Digest finish() noexcept;

  [[nodiscard]] static Digest compute(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] static Digest compute(std::string_view text) noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::uint32_t state_[8];
  std::uint64_t total_bytes_;
  std::uint8_t buffer_[64];
  std::size_t buffered_;
};

/// 64-bit FNV-1a. Fast, non-cryptographic; suitable for state checksums.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::span<const std::uint8_t> data,
                                              std::uint64_t seed = 0xcbf29ce484222325ull) noexcept {
  std::uint64_t h = seed;
  for (const std::uint8_t b : data) {
    h ^= b;
    h *= 0x100000001b3ull;
  }
  return h;
}

/// Lower-case hexadecimal rendering of a byte sequence.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

/// Parses a hexadecimal string (any case, no separators) into bytes.
/// Returns false if the input is not valid hex or does not fit `out`.
[[nodiscard]] bool from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept;

}  // namespace ttt2::hash
