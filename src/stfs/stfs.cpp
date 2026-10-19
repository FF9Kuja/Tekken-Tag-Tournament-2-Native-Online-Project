#include "ttt2/stfs/stfs.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "ttt2/core/endian.hpp"

namespace ttt2::stfs {

namespace {

constexpr std::uint32_t kEndOfChain = 0xFFFFFF;
constexpr std::uint64_t kFirstDataBlockOffset = 0xC000;  // backing block 0
constexpr std::size_t kFileEntrySize = 0x40;
constexpr std::size_t kHashEntrySize = 0x18;

std::uint32_t le24(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16);
}
std::uint32_t be24(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
         static_cast<std::uint32_t>(p[2]);
}

char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool names_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  }
  return true;
}

std::string utf16be_to_utf8(const std::uint8_t* p, std::size_t bytes) {
  std::string out;
  for (std::size_t i = 0; i + 1 < bytes; i += 2) {
    const std::uint16_t cu = load_be16(p + i);
    if (cu == 0) break;
    if (cu < 0x80) {
      out.push_back(static_cast<char>(cu));
    } else if (cu < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cu >> 6)));
      out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (cu >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cu >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    }
  }
  return out;
}

}  // namespace

std::uint32_t backing_block(std::uint32_t block, std::uint32_t table_shift) noexcept {
  std::uint32_t adjust = 0;
  if (block >= kBlocksPerHashTable) adjust += ((block / kBlocksPerHashTable) + 1) << table_shift;
  if (block >= kBlocksPerLevel1) adjust += ((block / kBlocksPerLevel1) + 1) << table_shift;
  return block + adjust;
}

std::uint64_t block_offset(std::uint32_t block, std::uint32_t table_shift) noexcept {
  return kFirstDataBlockOffset + (static_cast<std::uint64_t>(backing_block(block, table_shift)) << 12);
}

std::uint64_t hash_entry_offset(std::uint32_t block, std::uint32_t table_shift) noexcept {
  // The level-0 table of a run of 0xAA data blocks sits immediately before the run.
  const std::uint32_t run_first = block - (block % kBlocksPerHashTable);
  const std::uint64_t table = block_offset(run_first, table_shift) - kBlockSize;
  return table + static_cast<std::uint64_t>(block % kBlocksPerHashTable) * kHashEntrySize;
}

Expected<Package> Package::open(ByteSource& source, const OpenOptions& options) {
  std::uint8_t h[0x1000];
  if (source.size() < sizeof(h) || !source.read_at(0, h)) {
    return make_error("file is too small to be an STFS package");
  }
  PackageInfo info;
  const std::uint32_t magic = load_be32(h);
  if (magic != static_cast<std::uint32_t>(Magic::Live) && magic != static_cast<std::uint32_t>(Magic::Pirs) &&
      magic != static_cast<std::uint32_t>(Magic::Con)) {
    return make_error("not an STFS package (no LIVE / PIRS / CON magic)");
  }
  info.magic = static_cast<Magic>(magic);
  info.header_size = load_be32(h + 0x340);
  info.content_type = load_be32(h + 0x344);
  info.metadata_version = load_be32(h + 0x348);
  info.media_id = load_be32(h + 0x354);
  info.version = load_be32(h + 0x358);
  info.base_version = load_be32(h + 0x35C);
  info.title_id = load_be32(h + 0x360);
  info.platform = h[0x364];
  info.executable_type = h[0x365];
  info.disc_number = h[0x366];
  info.disc_in_set = h[0x367];
  info.save_game_id = load_be32(h + 0x368);
  // Volume descriptor at 0x379.
  const std::uint8_t descriptor_size = h[0x379];
  if (descriptor_size != 0x24) {
    return make_error("unexpected volume descriptor size " + std::to_string(descriptor_size));
  }
  info.block_separation = h[0x37A];
  info.file_table_block_count = static_cast<std::uint16_t>(h[0x37B] | (h[0x37C] << 8));  // little-endian
  info.file_table_block = le24(h + 0x37D);
  info.total_allocated_blocks = load_be32(h + 0x394);
  info.total_unallocated_blocks = load_be32(h + 0x398);
  info.data_file_count = load_be32(h + 0x39D);
  info.data_file_size = load_be64(h + 0x3A1);
  info.descriptor_type = load_be32(h + 0x3A9);
  info.display_name = utf16be_to_utf8(h + 0x411, 0x80);
  // One hash-table block per run when the header rounds up to 0xB000, two otherwise.
  info.table_shift = (((info.header_size + 0xFFF) & 0xF000) == 0xB000) ? 0 : 1;
  if (info.descriptor_type != 0) return make_error("SVOD packages are not supported (descriptor type 1)");
  if (info.file_table_block_count > options.max_file_table_blocks) {
    return make_error("file table block count " + std::to_string(info.file_table_block_count) +
                      " is implausible");
  }
  return Package(source, info, options);
}

Expected<std::uint32_t> Package::next_block(std::uint32_t block) {
  std::uint8_t entry[kHashEntrySize];
  const std::uint64_t off = hash_entry_offset(block, info_.table_shift);
  if (!source_->read_at(off, entry))
    return make_error("hash table entry for block " + std::to_string(block) + " is outside the file");
  return be24(entry + 0x15);
}

Expected<std::vector<FileEntry>> Package::list() {
  std::vector<FileEntry> entries;
  std::uint32_t block = info_.file_table_block;
  std::unordered_set<std::uint32_t> visited;
  for (std::uint32_t i = 0; i < info_.file_table_block_count; ++i) {
    if (block == kEndOfChain || !visited.insert(block).second) break;
    std::uint8_t data[kBlockSize];
    if (!source_->read_at(block_offset(block, info_.table_shift), data)) {
      return make_error("file table block " + std::to_string(block) + " is outside the file");
    }
    for (std::size_t e = 0; e + kFileEntrySize <= kBlockSize; e += kFileEntrySize) {
      const std::uint8_t* rec = data + e;
      const std::uint8_t flags = rec[0x28];
      const std::size_t name_length = flags & 0x3F;
      if (name_length == 0) continue;  // unused slot
      FileEntry f;
      f.name.assign(reinterpret_cast<const char*>(rec), std::min<std::size_t>(name_length, 0x28));
      f.is_directory = (flags & 0x80) != 0;
      f.consecutive = (flags & 0x40) != 0;
      f.allocated_blocks = le24(rec + 0x29);
      f.start_block = le24(rec + 0x2F);
      f.parent_index = static_cast<std::int16_t>(load_be16(rec + 0x32));
      f.size = load_be32(rec + 0x34);
      entries.push_back(std::move(f));
    }
    auto next = next_block(block);
    if (!next) return next.error();
    block = *next;
  }
  return entries;
}

std::string Package::path_of(const std::vector<FileEntry>& entries, std::size_t index) const {
  std::string path;
  std::size_t guard = 0;
  std::size_t i = index;
  while (i < entries.size() && guard++ < 64) {
    path = path.empty() ? entries[i].name : entries[i].name + "/" + path;
    const std::int16_t parent = entries[i].parent_index;
    if (parent < 0 || static_cast<std::size_t>(parent) >= entries.size()) break;
    i = static_cast<std::size_t>(parent);
  }
  return path;
}

Expected<FileEntry> Package::find(std::string_view name_or_path) {
  auto entries = list();
  if (!entries) return entries.error();
  for (std::size_t i = 0; i < entries->size(); ++i) {
    const auto& e = (*entries)[i];
    if (e.is_directory) continue;
    if (names_equal(e.name, name_or_path) || names_equal(path_of(*entries, i), name_or_path)) return e;
  }
  return make_error("not found in the package: " + std::string(name_or_path));
}

Expected<std::uint64_t> Package::stream_file(const FileEntry& entry,
                                             const std::function<bool(std::span<const std::uint8_t>)>& sink) {
  if (entry.is_directory) return make_error(entry.name + " is a directory");
  std::uint64_t remaining = entry.size;
  std::uint32_t block = entry.start_block;
  std::uint32_t count = 0;
  std::unordered_set<std::uint32_t> visited;
  std::uint8_t data[kBlockSize];
  while (remaining > 0) {
    if (block == kEndOfChain) return make_error(entry.name + ": block chain ends before the file size");
    if (count > entry.allocated_blocks + 1 || !visited.insert(block).second) {
      return make_error(entry.name + ": block chain is longer than its allocation or loops");
    }
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(kBlockSize, remaining));
    if (!source_->read_at(block_offset(block, info_.table_shift), std::span<std::uint8_t>(data, take))) {
      return make_error(entry.name + ": block " + std::to_string(block) + " is outside the file");
    }
    if (!sink(std::span<const std::uint8_t>(data, take))) return make_error("sink aborted");
    remaining -= take;
    ++count;
    if (remaining == 0) break;
    if (entry.consecutive) {
      ++block;
    } else {
      auto next = next_block(block);
      if (!next) return next.error();
      block = *next;
    }
  }
  return static_cast<std::uint64_t>(entry.size);
}

Expected<std::vector<std::uint8_t>> Package::read_file(const FileEntry& entry, std::size_t max_bytes) {
  if (entry.size > max_bytes) {
    return make_error(entry.name + " is " + std::to_string(entry.size) + " bytes, above the " +
                      std::to_string(max_bytes) + "-byte limit");
  }
  std::vector<std::uint8_t> out;
  out.reserve(entry.size);
  auto n = stream_file(entry, [&](std::span<const std::uint8_t> chunk) {
    out.insert(out.end(), chunk.begin(), chunk.end());
    return true;
  });
  if (!n) return n.error();
  return out;
}

}  // namespace ttt2::stfs
