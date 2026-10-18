// SPDX-License-Identifier: (see LICENSE decision in docs/LEGAL.md)
//
// Big-endian helpers.
//
// The Xbox 360 (Xenon) is a big-endian PowerPC machine. Every byte of guest
// memory and every file format we touch (XEX2 executables, STFS title-update
// containers, XDVDFS disc images) stores multi-byte integers big-endian, while
// the Windows/Linux x86-64 host is little-endian. Everything that crosses that
// boundary goes through these helpers so the swap happens in exactly one place.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ttt2 {

[[nodiscard]] constexpr std::uint16_t byteswap16(std::uint16_t v) noexcept {
  return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}

[[nodiscard]] constexpr std::uint32_t byteswap32(std::uint32_t v) noexcept {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

[[nodiscard]] constexpr std::uint64_t byteswap64(std::uint64_t v) noexcept {
  return (static_cast<std::uint64_t>(byteswap32(static_cast<std::uint32_t>(v))) << 32) |
         byteswap32(static_cast<std::uint32_t>(v >> 32));
}

namespace detail {

template <typename T>
using unsigned_bits_t =
    std::conditional_t<sizeof(T) == 1, std::uint8_t,
                       std::conditional_t<sizeof(T) == 2, std::uint16_t,
                                          std::conditional_t<sizeof(T) == 4, std::uint32_t,
                                                             std::uint64_t>>>;

template <typename U>
[[nodiscard]] constexpr U byteswap_unsigned(U v) noexcept {
  if constexpr (sizeof(U) == 1) {
    return v;
  } else if constexpr (sizeof(U) == 2) {
    return byteswap16(v);
  } else if constexpr (sizeof(U) == 4) {
    return byteswap32(v);
  } else {
    return byteswap64(v);
  }
}

}  // namespace detail

/// Reverses the byte order of any 1/2/4/8-byte integral or enum value.
template <typename T>
[[nodiscard]] constexpr T byteswap(T v) noexcept {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                "byteswap requires an integral or enum type");
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
  using U = detail::unsigned_bits_t<T>;
  return std::bit_cast<T>(detail::byteswap_unsigned(std::bit_cast<U>(v)));
}

/// Converts a host-order value to its big-endian bit pattern (no-op on big-endian hosts).
template <typename T>
[[nodiscard]] constexpr T host_to_be(T v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    return v;
  } else {
    return byteswap(v);
  }
}

/// Converts a big-endian bit pattern to a host-order value (no-op on big-endian hosts).
template <typename T>
[[nodiscard]] constexpr T be_to_host(T v) noexcept {
  return host_to_be(v);  // the swap is an involution
}

// ---------------------------------------------------------------------------
// Unaligned loads / stores from byte buffers. These never assume alignment and
// are the preferred way to read file images (which may be mapped at any
// offset) and guest memory.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint8_t load_be8(const std::uint8_t* p) noexcept { return p[0]; }

[[nodiscard]] constexpr std::uint16_t load_be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] constexpr std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(load_be32(p)) << 32) | load_be32(p + 4);
}

constexpr void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}

constexpr void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

constexpr void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  store_be32(p, static_cast<std::uint32_t>(v >> 32));
  store_be32(p + 4, static_cast<std::uint32_t>(v));
}

/// A big-endian storage wrapper for overlaying C++ structs onto guest memory
/// or file images. Reads and writes transparently convert to host order.
/// The object is trivially copyable and exactly sizeof(T) bytes.
template <typename T>
struct be {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                "be<T> requires an integral or enum type");

  T raw{};  ///< the big-endian bit pattern as stored in memory

  constexpr be() noexcept = default;
  constexpr be(T host_value) noexcept : raw(host_to_be(host_value)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] constexpr T get() const noexcept { return be_to_host(raw); }
  constexpr operator T() const noexcept { return get(); }  // NOLINT(google-explicit-constructor)

  constexpr be& operator=(T host_value) noexcept {
    raw = host_to_be(host_value);
    return *this;
  }
};

static_assert(sizeof(be<std::uint32_t>) == 4);
static_assert(std::is_trivially_copyable_v<be<std::uint32_t>>);

}  // namespace ttt2
