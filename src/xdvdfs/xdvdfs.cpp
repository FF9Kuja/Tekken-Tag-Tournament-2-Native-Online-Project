#include "ttt2/xdvdfs/xdvdfs.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace ttt2::xdvdfs {

namespace {

constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr std::size_t kMagicSize = 20;

std::uint16_t le16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) noexcept {
  return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/' || path[i] == '\\') {
      if (i > start) parts.push_back(path.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

}  // namespace

bool names_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  }
  return true;
}

Expected<Image> Image::open(ByteSource& source, const OpenOptions& options) {
  for (const std::uint64_t base : kPartitionOffsets) {
    const std::uint64_t descriptor = base + static_cast<std::uint64_t>(kVolumeDescriptorSector) * kSectorSize;
    if (descriptor + kSectorSize > source.size()) continue;
    std::uint8_t sector[kSectorSize];
    if (!source.read_at(descriptor, sector)) continue;
    if (std::memcmp(sector, kMagic, kMagicSize) != 0) continue;
    if (std::memcmp(sector + 0x7EC, kMagic, kMagicSize) != 0) continue;
    VolumeInfo v;
    v.partition_offset = base;
    v.root_sector = le32(sector + 20);
    v.root_size = le32(sector + 24);
    v.filetime = le64(sector + 28);
    return Image(source, v, options);
  }
  return make_error(
      "no XDVDFS volume descriptor found at any known partition offset (not an Xbox 360 disc image?)");
}

Expected<std::vector<Entry>> Image::read_directory(std::uint32_t sector, std::uint32_t size) {
  std::vector<Entry> entries;
  if (size == 0) return entries;  // an empty directory has no table
  if (size > options_.max_directory_bytes) {
    return make_error("directory table of " + std::to_string(size) + " bytes exceeds the limit");
  }
  const std::uint64_t offset = volume_.partition_offset + static_cast<std::uint64_t>(sector) * kSectorSize;
  auto table = source_->read(offset, size);
  if (!table) return make_error("directory table is outside the image");
  const std::vector<std::uint8_t>& t = table.value();

  // In-order traversal of the binary tree rooted at offset 0 (offsets are in dwords).
  std::unordered_set<std::uint32_t> visited;
  struct Frame {
    std::uint32_t offset;
    bool expanded;
  };
  std::vector<Frame> stack;
  stack.push_back({0, false});
  while (!stack.empty()) {
    Frame frame = stack.back();
    stack.pop_back();
    const std::uint32_t off = frame.offset;
    if (off + 14 > t.size())
      return make_error("directory entry at offset " + std::to_string(off) + " is truncated");
    const std::uint16_t left = le16(t.data() + off);
    const std::uint16_t right = le16(t.data() + off + 2);
    if (left == 0xFFFF && right == 0xFFFF) continue;  // sector padding
    if (!frame.expanded) {
      if (!visited.insert(off).second) return make_error("directory tree contains a cycle");
      if (visited.size() > options_.max_entries_per_directory)
        return make_error("directory has too many entries");
      // Visit order: left subtree, this node, right subtree.
      if (right != 0) stack.push_back({static_cast<std::uint32_t>(right) * 4, false});
      stack.push_back({off, true});
      if (left != 0) stack.push_back({static_cast<std::uint32_t>(left) * 4, false});
      continue;
    }
    Entry e;
    e.start_sector = le32(t.data() + off + 4);
    e.size = le32(t.data() + off + 8);
    e.attributes = t[off + 12];
    const std::uint8_t name_length = t[off + 13];
    if (off + 14u + name_length > t.size()) return make_error("directory entry name is truncated");
    e.name.assign(reinterpret_cast<const char*>(t.data() + off + 14), name_length);
    entries.push_back(std::move(e));
  }
  return entries;
}

Expected<std::vector<Entry>> Image::list(std::string_view directory_path) {
  const auto parts = split_path(directory_path);
  std::uint32_t sector = volume_.root_sector;
  std::uint32_t size = volume_.root_size;
  for (const auto part : parts) {
    auto entries = read_directory(sector, size);
    if (!entries) return entries.error();
    const auto it = std::find_if(entries->begin(), entries->end(),
                                 [&](const Entry& e) { return names_equal(e.name, part); });
    if (it == entries->end()) return make_error("directory not found: " + std::string(part));
    if (!it->is_directory()) return make_error("not a directory: " + std::string(part));
    sector = it->start_sector;
    size = it->size;
  }
  return read_directory(sector, size);
}

Expected<Entry> Image::find(std::string_view path) {
  const auto parts = split_path(path);
  if (parts.empty()) return make_error("empty path");
  std::uint32_t sector = volume_.root_sector;
  std::uint32_t size = volume_.root_size;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    auto entries = read_directory(sector, size);
    if (!entries) return entries.error();
    const auto it = std::find_if(entries->begin(), entries->end(),
                                 [&](const Entry& e) { return names_equal(e.name, parts[i]); });
    if (it == entries->end()) return make_error("not found: " + std::string(parts[i]));
    if (i + 1 == parts.size()) return *it;
    if (!it->is_directory()) return make_error("not a directory: " + std::string(parts[i]));
    sector = it->start_sector;
    size = it->size;
  }
  return make_error("empty path");
}

Expected<std::vector<std::uint8_t>> Image::read_file(const Entry& entry, std::size_t max_bytes) {
  if (entry.is_directory()) return make_error(entry.name + " is a directory");
  if (entry.size > max_bytes) {
    return make_error(entry.name + " is " + std::to_string(entry.size) + " bytes, above the " +
                      std::to_string(max_bytes) + "-byte limit");
  }
  auto data = source_->read(data_offset(entry), entry.size);
  if (!data) return make_error(entry.name + " lies outside the image");
  return data;
}

Expected<std::uint64_t> Image::stream_file(const Entry& entry,
                                           const std::function<bool(std::span<const std::uint8_t>)>& sink,
                                           std::size_t chunk_size) {
  if (entry.is_directory()) return make_error(entry.name + " is a directory");
  if (chunk_size == 0) return make_error("chunk size must be positive");
  std::vector<std::uint8_t> buffer(std::min<std::size_t>(chunk_size, entry.size ? entry.size : 1));
  std::uint64_t done = 0;
  const std::uint64_t base = data_offset(entry);
  while (done < entry.size) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), entry.size - done));
    if (!source_->read_at(base + done, std::span<std::uint8_t>(buffer.data(), n))) {
      return make_error(entry.name + " lies outside the image");
    }
    if (!sink(std::span<const std::uint8_t>(buffer.data(), n))) return make_error("sink aborted");
    done += n;
  }
  return done;
}

}  // namespace ttt2::xdvdfs
