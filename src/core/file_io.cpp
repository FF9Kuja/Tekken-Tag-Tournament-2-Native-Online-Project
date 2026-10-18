#include "ttt2/core/file_io.hpp"

#include <fstream>
#include <system_error>

namespace ttt2 {

Expected<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                              std::uint64_t max_bytes) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return make_error("not a regular file: " + path.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return make_error("cannot stat " + path.string() + ": " + ec.message());
  if (size > max_bytes) {
    return make_error("file is larger than the " + std::to_string(max_bytes) +
                      "-byte limit: " + path.string());
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return make_error("cannot open " + path.string());
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
    return make_error("short read: " + path.string());
  }
  return data;
}

}  // namespace ttt2
