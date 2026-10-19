// Builds synthetic XDVDFS disc images in memory for tests (no game data).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ttt2::test {

class XdvdfsBuilder {
 public:
  struct File {
    std::string path;  // "dir/sub/name.ext"
    std::vector<std::uint8_t> data;
  };

  explicit XdvdfsBuilder(std::uint64_t partition_offset = 0) : partition_offset_(partition_offset) {}

  void add_file(std::string path, std::vector<std::uint8_t> data) {
    files_.push_back({std::move(path), std::move(data)});
  }

  std::vector<std::uint8_t> build() {
    // Directory tree from the paths.
    Dir root;
    for (const auto& f : files_) {
      Dir* d = &root;
      std::size_t start = 0;
      for (;;) {
        const std::size_t slash = f.path.find('/', start);
        if (slash == std::string::npos) break;
        const std::string part = f.path.substr(start, slash - start);
        d = &d->subdir(part);
        start = slash + 1;
      }
      d->files.push_back({f.path.substr(start), f.data});
    }
    // Lay out: sector 0..31 reserved, 32 = descriptor, then tables and data.
    next_sector_ = 33;
    std::vector<std::uint8_t> image;
    image.resize(static_cast<std::size_t>(partition_offset_) + 34 * 2048);
    const Placed root_placed = place(root, image);
    // Volume descriptor.
    std::uint8_t* desc = image.data() + partition_offset_ + 32 * 2048;
    std::memcpy(desc, "MICROSOFT*XBOX*MEDIA", 20);
    store_le32(desc + 20, root_placed.sector);
    store_le32(desc + 24, root_placed.size);
    store_le64(desc + 28, 0x01D0000000000000ull);
    std::memcpy(desc + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    return image;
  }

 private:
  struct Dir {
    std::vector<std::string> subdir_names;  // parallel to subdirs (Dir is incomplete inside itself)
    std::vector<Dir> subdirs;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    Dir& subdir(const std::string& name) {
      for (std::size_t i = 0; i < subdir_names.size(); ++i) {
        if (subdir_names[i] == name) return subdirs[i];
      }
      subdir_names.push_back(name);
      subdirs.emplace_back();
      return subdirs.back();
    }
  };
  struct Placed {
    std::uint32_t sector;
    std::uint32_t size;
  };
  struct Node {
    std::string name;
    std::uint32_t sector;
    std::uint32_t size;
    std::uint8_t attributes;
  };

  static void store_le16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  }
  static void store_le32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  static void store_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  static char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
  static bool less_ci(const std::string& a, const std::string& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      if (lower(a[i]) != lower(b[i])) return lower(a[i]) < lower(b[i]);
    }
    return a.size() < b.size();
  }

  std::uint32_t append_data(std::vector<std::uint8_t>& image, const std::vector<std::uint8_t>& data) {
    const std::uint32_t sector = next_sector_;
    const std::size_t sectors = (data.size() + 2047) / 2048;
    const std::size_t off =
        static_cast<std::size_t>(partition_offset_) + static_cast<std::size_t>(sector) * 2048;
    if (image.size() < off + sectors * 2048) image.resize(off + sectors * 2048);
    std::memcpy(image.data() + off, data.data(), data.size());
    next_sector_ += static_cast<std::uint32_t>(sectors ? sectors : 1);
    return sector;
  }

  // Writes the subtree [lo, hi) of sorted nodes at `pos`; returns the dword offset of its root.
  std::uint16_t write_tree(std::vector<std::uint8_t>& table, const std::vector<Node>& nodes, std::size_t lo,
                           std::size_t hi) {
    if (lo >= hi) return 0;
    const std::size_t mid = (lo + hi) / 2;
    const Node& n = nodes[mid];
    const std::size_t entry_off = table.size();
    const std::size_t entry_size = (14 + n.name.size() + 3) & ~static_cast<std::size_t>(3);
    table.resize(entry_off + entry_size, 0);
    std::uint8_t* e = table.data() + entry_off;
    store_le32(e + 4, n.sector);
    store_le32(e + 8, n.size);
    e[12] = n.attributes;
    e[13] = static_cast<std::uint8_t>(n.name.size());
    std::memcpy(e + 14, n.name.data(), n.name.size());
    const std::uint16_t left = write_tree(table, nodes, lo, mid);
    const std::uint16_t right = write_tree(table, nodes, mid + 1, hi);
    store_le16(table.data() + entry_off, left);
    store_le16(table.data() + entry_off + 2, right);
    return static_cast<std::uint16_t>(entry_off / 4);
  }

  Placed place(const Dir& dir, std::vector<std::uint8_t>& image) {
    std::vector<Node> nodes;
    for (std::size_t i = 0; i < dir.subdirs.size(); ++i) {
      const Placed p = place(dir.subdirs[i], image);
      nodes.push_back({dir.subdir_names[i], p.sector, p.size, 0x10});
    }
    for (const auto& [name, data] : dir.files) {
      const std::uint32_t sector = append_data(image, data);
      nodes.push_back({name, sector, static_cast<std::uint32_t>(data.size()), 0x80});
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const Node& a, const Node& b) { return less_ci(a.name, b.name); });
    std::vector<std::uint8_t> table;
    write_tree(table, nodes, 0, nodes.size());
    // Pad the table to a whole sector with 0xFF (as real images do).
    const std::size_t padded = ((table.size() + 2047) / 2048) * 2048;
    table.resize(padded ? padded : 2048, 0xFF);
    const std::uint32_t sector = append_data(image, table);
    return {sector, static_cast<std::uint32_t>(table.size())};
  }

  std::uint64_t partition_offset_;
  std::vector<File> files_;
  std::uint32_t next_sector_ = 33;
};

}  // namespace ttt2::test
