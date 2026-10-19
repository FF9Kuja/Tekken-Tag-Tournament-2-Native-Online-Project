// Random-access byte sources for large inputs (disc images are 7 GB+ and
// cannot be loaded into memory).

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "ttt2/core/expected.hpp"

namespace ttt2 {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  /// Reads exactly `out.size()` bytes at `offset`; false if out of range or on I/O error.
  [[nodiscard]] virtual bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept = 0;

  /// Convenience: reads `length` bytes into a vector (nullopt on failure).
  [[nodiscard]] Expected<std::vector<std::uint8_t>> read(std::uint64_t offset, std::size_t length);
};

/// A source over an in-memory buffer (tests, small inputs).
class MemorySource final : public ByteSource {
 public:
  explicit MemorySource(std::span<const std::uint8_t> data) noexcept : data_(data) {}
  [[nodiscard]] std::uint64_t size() const noexcept override { return data_.size(); }
  [[nodiscard]] bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override;

 private:
  std::span<const std::uint8_t> data_;
};

/// A source over a file on disk, using positional reads.
class FileSource final : public ByteSource {
 public:
  [[nodiscard]] static Expected<std::unique_ptr<FileSource>> open(const std::filesystem::path& path);
  ~FileSource() override;
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
  [[nodiscard]] bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override;

 private:
  FileSource(std::FILE* file, std::uint64_t size) noexcept : file_(file), size_(size) {}
  std::FILE* file_;
  std::uint64_t size_;
};

}  // namespace ttt2
