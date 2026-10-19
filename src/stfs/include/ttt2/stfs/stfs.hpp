// STFS package reader (Xbox 360 "LIVE" / "PIRS" / "CON " containers).
//
// Title updates are distributed as STFS packages: a signed header followed by
// 4 KiB blocks. Data blocks are interleaved with SHA-1 hash-table blocks (one
// level-0 table per 0xAA data blocks, one level-1 table per 0xAA level-0
// tables, ...), and the hash entries double as the "next block" chain for
// files. Multi-byte integers are big-endian except a few descriptor fields,
// which are little-endian 16/24-bit values.
//
// Written from the public format description (free60 "STFS", py360/wxPirs
// block addressing). This project needs it to pull `Default.xexp` and the
// `data100.bin/.ofs` archives out of Tekken Tag Tournament 2's Title Update 3
// on the player's machine. Reads are bounds-checked through ByteSource.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/expected.hpp"

namespace ttt2::stfs {

inline constexpr std::uint32_t kBlockSize = 0x1000;
inline constexpr std::uint32_t kBlocksPerHashTable = 0xAA;
inline constexpr std::uint32_t kBlocksPerLevel1 = 0xAA * 0xAA;  // 0x70E4
inline constexpr std::uint32_t kContentTypeTitleUpdate = 0x000B0000;

enum class Magic : std::uint32_t { Con = 0x434F4E20, Live = 0x4C495645, Pirs = 0x50495253 };

struct PackageInfo {
  Magic magic = Magic::Live;
  std::uint32_t header_size = 0;
  std::uint32_t content_type = 0;
  std::uint32_t metadata_version = 0;
  std::uint32_t media_id = 0;
  std::uint32_t version = 0;  ///< packed XEX-style version
  std::uint32_t base_version = 0;
  std::uint32_t title_id = 0;
  std::uint8_t platform = 0;
  std::uint8_t executable_type = 0;
  std::uint8_t disc_number = 0;
  std::uint8_t disc_in_set = 0;
  std::uint32_t save_game_id = 0;
  // volume descriptor
  std::uint8_t block_separation = 0;
  std::uint16_t file_table_block_count = 0;
  std::uint32_t file_table_block = 0;
  std::uint32_t total_allocated_blocks = 0;
  std::uint32_t total_unallocated_blocks = 0;
  std::uint32_t data_file_count = 0;
  std::uint64_t data_file_size = 0;
  std::uint32_t descriptor_type = 0;  ///< 0 STFS, 1 SVOD
  std::uint32_t table_shift = 0;      ///< 0: one block per hash table, 1: two
  std::string display_name;           ///< first display name (UTF-16BE converted to UTF-8)

  [[nodiscard]] bool is_title_update() const noexcept { return content_type == kContentTypeTitleUpdate; }
};

struct FileEntry {
  std::string name;
  std::uint32_t size = 0;
  std::uint32_t start_block = 0;
  std::uint32_t allocated_blocks = 0;
  std::int16_t parent_index = -1;  ///< index of the parent directory entry, -1 = root
  bool is_directory = false;
  bool consecutive = false;  ///< blocks are sequential (no chain walk needed)
};

struct OpenOptions {
  std::uint32_t max_file_table_blocks = 256;
};

/// Block number arithmetic (exposed for tests).
[[nodiscard]] std::uint32_t backing_block(std::uint32_t block, std::uint32_t table_shift) noexcept;
[[nodiscard]] std::uint64_t block_offset(std::uint32_t block, std::uint32_t table_shift) noexcept;
/// File offset of the level-0 hash entry (0x18 bytes) describing `block`.
[[nodiscard]] std::uint64_t hash_entry_offset(std::uint32_t block, std::uint32_t table_shift) noexcept;

class Package {
 public:
  [[nodiscard]] static Expected<Package> open(ByteSource& source, const OpenOptions& options = {});

  [[nodiscard]] const PackageInfo& info() const noexcept { return info_; }

  /// All file-table entries (files and directories) in table order.
  [[nodiscard]] Expected<std::vector<FileEntry>> list();

  /// Full path of an entry ("dir/sub/name"), resolved through parent indices.
  [[nodiscard]] std::string path_of(const std::vector<FileEntry>& entries, std::size_t index) const;

  /// Finds a file by name or path (case-insensitive).
  [[nodiscard]] Expected<FileEntry> find(std::string_view name_or_path);

  [[nodiscard]] Expected<std::vector<std::uint8_t>> read_file(const FileEntry& entry, std::size_t max_bytes);
  [[nodiscard]] Expected<std::uint64_t> stream_file(
      const FileEntry& entry, const std::function<bool(std::span<const std::uint8_t>)>& sink);

  /// The next block in a chain (0xFFFFFF = end).
  [[nodiscard]] Expected<std::uint32_t> next_block(std::uint32_t block);

 private:
  Package(ByteSource& source, PackageInfo info, OpenOptions options) noexcept
      : source_(&source), info_(info), options_(options) {}

  ByteSource* source_;
  PackageInfo info_;
  OpenOptions options_;
};

}  // namespace ttt2::stfs
