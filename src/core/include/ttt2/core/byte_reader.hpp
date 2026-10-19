// Bounds-checked big-endian reader over an immutable byte buffer.
//
// All file-format parsers in this project read through ByteReader so that a
// malformed or truncated input can never cause an out-of-bounds read: every
// accessor returns std::nullopt instead of reading past the end.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "ttt2/core/endian.hpp"

namespace ttt2 {

class ByteReader {
 public:
  constexpr ByteReader() noexcept = default;
  constexpr explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] constexpr std::span<const std::uint8_t> data() const noexcept { return data_; }

  /// True when [offset, offset + length) lies entirely inside the buffer.
  [[nodiscard]] constexpr bool in_bounds(std::size_t offset, std::size_t length) const noexcept {
    return offset <= data_.size() && length <= data_.size() - offset;
  }

  [[nodiscard]] constexpr std::optional<std::uint8_t> u8(std::size_t offset) const noexcept {
    if (!in_bounds(offset, 1)) return std::nullopt;
    return data_[offset];
  }

  [[nodiscard]] constexpr std::optional<std::uint16_t> u16be(std::size_t offset) const noexcept {
    if (!in_bounds(offset, 2)) return std::nullopt;
    return load_be16(data_.data() + offset);
  }

  [[nodiscard]] constexpr std::optional<std::uint32_t> u32be(std::size_t offset) const noexcept {
    if (!in_bounds(offset, 4)) return std::nullopt;
    return load_be32(data_.data() + offset);
  }

  [[nodiscard]] constexpr std::optional<std::uint64_t> u64be(std::size_t offset) const noexcept {
    if (!in_bounds(offset, 8)) return std::nullopt;
    return load_be64(data_.data() + offset);
  }

  /// A sub-span of the buffer, or nullopt if it would run past the end.
  [[nodiscard]] constexpr std::optional<std::span<const std::uint8_t>> bytes(
      std::size_t offset, std::size_t length) const noexcept {
    if (!in_bounds(offset, length)) return std::nullopt;
    return data_.subspan(offset, length);
  }

  /// A NUL-terminated string starting at `offset`, searched within at most
  /// `max_length` bytes. Returns the bytes before the NUL (or all
  /// `max_length` bytes when no NUL is found inside the window).
  [[nodiscard]] std::optional<std::string> cstring(std::size_t offset, std::size_t max_length) const {
    if (!in_bounds(offset, 0)) return std::nullopt;
    const std::size_t available = data_.size() - offset;
    const std::size_t window = max_length < available ? max_length : available;
    std::size_t len = 0;
    while (len < window && data_[offset + len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(data_.data() + offset), len);
  }

 private:
  std::span<const std::uint8_t> data_{};
};

}  // namespace ttt2
