#include "ttt2/core/byte_source.hpp"

#include <cstring>
#include <system_error>

namespace ttt2 {

Expected<std::vector<std::uint8_t>> ByteSource::read(std::uint64_t offset, std::size_t length) {
  std::vector<std::uint8_t> out(length);
  if (!read_at(offset, out)) {
    return make_error("read of " + std::to_string(length) + " bytes at offset " + std::to_string(offset) +
                      " failed");
  }
  return out;
}

bool MemorySource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
  if (offset > data_.size() || out.size() > data_.size() - offset) return false;
  std::memcpy(out.data(), data_.data() + offset, out.size());
  return true;
}

Expected<std::unique_ptr<FileSource>> FileSource::open(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return make_error("cannot stat " + path.string() + ": " + ec.message());
#if defined(_WIN32)
  std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
  std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) return make_error("cannot open " + path.string());
  return std::unique_ptr<FileSource>(new FileSource(f, size));
}

FileSource::~FileSource() {
  if (file_) std::fclose(file_);
}

bool FileSource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
  if (offset > size_ || out.size() > size_ - offset) return false;
  if (out.empty()) return true;
#if defined(_WIN32)
  if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
  if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
  return std::fread(out.data(), 1, out.size(), file_) == out.size();
}

}  // namespace ttt2
