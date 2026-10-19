// Minimal "value or error" result type.
//
// std::expected is C++23 and not yet uniformly available across the compilers
// this project supports (MSVC 2022, GCC 13, Clang 18, Apple Clang), so a small
// local equivalent is used instead. Replace with std::expected once the
// toolchain floor allows it.

#pragma once

#include <string>
#include <utility>
#include <variant>

namespace ttt2 {

struct Error {
  std::string message;
};

[[nodiscard]] inline Error make_error(std::string message) {
  return Error{std::move(message)};
}

template <typename T>
class Expected {
 public:
  Expected(T value) : storage_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Expected(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Error& error() const { return std::get<1>(storage_); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<T, Error> storage_;
};

}  // namespace ttt2
