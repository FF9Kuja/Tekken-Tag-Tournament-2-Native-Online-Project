// XDVDFS (Xbox / Xbox 360 "GDF") disc image reader.
//
// The game partition of an Xbox 360 disc is a simple read-only file system:
// a volume descriptor at sector 32 ("MICROSOFT*XBOX*MEDIA"), and directories
// stored as binary search trees of entries (left/right links in dwords, start
// sector, size, attributes, name). All integers are little-endian. Depending on
// how a disc was dumped, the game partition starts at a different file offset
// (XGD3 redump 0x2080000, XGD2 0xFD90000, XGD1 0x18300000, or 0 for an
// extracted partition); the reader probes those.
//
// Written from the public format description (free60 / xboxdevwiki). Reads
// go through ByteSource so a 7 GB image is never loaded into memory, and every
// structure is bounds-checked.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/expected.hpp"

namespace ttt2::xdvdfs {

inline constexpr std::uint32_t kSectorSize = 2048;
inline constexpr std::uint32_t kVolumeDescriptorSector = 32;
inline constexpr std::uint64_t kPartitionOffsets[] = {0x2080000ull, 0xFD90000ull, 0ull, 0x18300000ull};

enum Attribute : std::uint8_t {
  kReadOnly = 0x01,
  kHidden = 0x02,
  kSystem = 0x04,
  kDirectory = 0x10,
  kArchive = 0x20,
  kNormal = 0x80,
};

struct Entry {
  std::string name;
  std::uint32_t start_sector = 0;  ///< relative to the partition
  std::uint32_t size = 0;          ///< bytes (directories: size of their table)
  std::uint8_t attributes = 0;

  [[nodiscard]] bool is_directory() const noexcept { return (attributes & kDirectory) != 0; }
};

struct VolumeInfo {
  std::uint64_t partition_offset = 0;
  std::uint32_t root_sector = 0;
  std::uint32_t root_size = 0;
  std::uint64_t filetime = 0;  ///< Windows FILETIME of the image
};

struct OpenOptions {
  std::uint32_t max_directory_bytes = 16u * 1024 * 1024;
  std::uint32_t max_entries_per_directory = 65536;
};

class Image {
 public:
  /// Probes the known partition offsets and reads the volume descriptor.
  [[nodiscard]] static Expected<Image> open(ByteSource& source, const OpenOptions& options = {});

  [[nodiscard]] const VolumeInfo& volume() const noexcept { return volume_; }

  /// Entries of a directory ("" or "/" for the root), in name order.
  [[nodiscard]] Expected<std::vector<Entry>> list(std::string_view directory_path);

  /// Looks a path up case-insensitively; '/' and '\\' both separate components.
  [[nodiscard]] Expected<Entry> find(std::string_view path);

  /// Absolute file offset of an entry's data inside the image.
  [[nodiscard]] std::uint64_t data_offset(const Entry& entry) const noexcept {
    return volume_.partition_offset + static_cast<std::uint64_t>(entry.start_sector) * kSectorSize;
  }

  /// Reads a whole file (refused above `max_bytes`).
  [[nodiscard]] Expected<std::vector<std::uint8_t>> read_file(const Entry& entry, std::size_t max_bytes);

  /// Streams a file in chunks to `sink` (for hashing / extracting large files).
  [[nodiscard]] Expected<std::uint64_t> stream_file(
      const Entry& entry, const std::function<bool(std::span<const std::uint8_t>)>& sink,
      std::size_t chunk_size = 1u << 20);

 private:
  Image(ByteSource& source, VolumeInfo volume, OpenOptions options) noexcept
      : source_(&source), volume_(volume), options_(options) {}

  [[nodiscard]] Expected<std::vector<Entry>> read_directory(std::uint32_t sector, std::uint32_t size);

  ByteSource* source_;
  VolumeInfo volume_;
  OpenOptions options_;
};

/// Case-insensitive ASCII comparison used for XDVDFS names.
[[nodiscard]] bool names_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace ttt2::xdvdfs
