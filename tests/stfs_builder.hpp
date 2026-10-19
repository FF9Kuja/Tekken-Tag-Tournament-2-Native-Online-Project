// Builds synthetic STFS ("LIVE") packages in memory for tests. No game data.
// Layout follows the public format: header padded to 0xB000 (table shift 0),
// one level-0 hash table block before every run of 0xAA data blocks, files
// chained through the hash entries' next-block fields.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ttt2/core/endian.hpp"

namespace ttt2::test {

class StfsBuilder {
 public:
  struct File {
    std::string name;
    std::vector<std::uint8_t> data;
    bool consecutive = false;
  };

  void set_title(std::uint32_t title_id, std::uint32_t content_type, std::uint32_t version) {
    title_id_ = title_id;
    content_type_ = content_type;
    version_ = version;
  }
  void set_display_name(std::string name) { display_name_ = std::move(name); }
  void add_file(std::string name, std::vector<std::uint8_t> data, bool consecutive = false) {
    files_.push_back({std::move(name), std::move(data), consecutive});
  }
  /// Allocate file blocks out of order (non-consecutive chains) when true.
  void set_scramble(bool scramble) { scramble_ = scramble; }

  std::vector<std::uint8_t> build() {
    // Assign data blocks: file table first (block 0), then files.
    struct Placed {
      std::vector<std::uint32_t> blocks;
    };
    std::uint32_t next = 1;
    std::vector<Placed> placed(files_.size());
    for (std::size_t i = 0; i < files_.size(); ++i) {
      const std::uint32_t n = static_cast<std::uint32_t>((files_[i].data.size() + 0xFFF) / 0x1000);
      for (std::uint32_t k = 0; k < n; ++k) placed[i].blocks.push_back(next++);
    }
    if (scramble_) {
      // Interleave the chains of the files so "next" is never block + 1.
      std::vector<std::uint32_t> pool;
      for (auto& p : placed) pool.insert(pool.end(), p.blocks.begin(), p.blocks.end());
      std::uint32_t cursor = 0;
      for (std::size_t round = 0; cursor < pool.size(); ++round) {
        for (auto& p : placed) {
          if (round < p.blocks.size()) p.blocks[round] = pool[cursor++];
        }
      }
    }
    const std::uint32_t total_blocks = next;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(offset_of(total_blocks)) + 0x1000, 0);

    // Header.
    std::memcpy(out.data(), "LIVE", 4);
    store_be32(out.data() + 0x340, 0xB000);  // header size -> table shift 0
    store_be32(out.data() + 0x344, content_type_);
    store_be32(out.data() + 0x348, 2);            // metadata version
    store_be32(out.data() + 0x354, 0x12345678u);  // media id
    store_be32(out.data() + 0x358, version_);
    store_be32(out.data() + 0x35C, 0);
    store_be32(out.data() + 0x360, title_id_);
    out[0x364] = 0xFF;
    out[0x379] = 0x24;  // descriptor size
    out[0x37A] = 0;     // block separation
    out[0x37B] = 1;     // file table block count (LE16)
    out[0x37C] = 0;
    out[0x37D] = 0;
    out[0x37E] = 0;
    out[0x37F] = 0;  // file table block 0 (LE24)
    store_be32(out.data() + 0x394, total_blocks);
    store_be32(out.data() + 0x39D, static_cast<std::uint32_t>(files_.size()));
    std::uint64_t total_size = 0;
    for (const auto& f : files_) total_size += f.data.size();
    store_be64(out.data() + 0x3A1, total_size);
    store_be32(out.data() + 0x3A9, 0);  // STFS
    for (std::size_t i = 0; i < display_name_.size() && i < 0x3F; ++i) {
      store_be16(out.data() + 0x411 + i * 2, static_cast<std::uint16_t>(display_name_[i]));
    }

    // Hash tables: next pointers default to end-of-chain.
    for (std::uint32_t b = 0; b < total_blocks; ++b) set_next(out, b, 0xFFFFFF);

    // File table in block 0.
    std::uint8_t* table = out.data() + offset_of(0);
    for (std::size_t i = 0; i < files_.size(); ++i) {
      std::uint8_t* rec = table + i * 0x40;
      const auto& f = files_[i];
      std::memcpy(rec, f.name.data(), f.name.size() < 0x28 ? f.name.size() : 0x28);
      rec[0x28] = static_cast<std::uint8_t>((f.name.size() & 0x3F) | (f.consecutive ? 0x40 : 0));
      const std::uint32_t n = static_cast<std::uint32_t>(placed[i].blocks.size());
      store_le24(rec + 0x29, n);
      store_le24(rec + 0x2C, n);
      store_le24(rec + 0x2F, placed[i].blocks.empty() ? 0 : placed[i].blocks[0]);
      store_be16(rec + 0x32, 0xFFFF);  // root
      store_be32(rec + 0x34, static_cast<std::uint32_t>(f.data.size()));
    }

    // File data and chains.
    for (std::size_t i = 0; i < files_.size(); ++i) {
      const auto& blocks = placed[i].blocks;
      for (std::size_t k = 0; k < blocks.size(); ++k) {
        const std::size_t src = k * 0x1000;
        const std::size_t n = std::min<std::size_t>(0x1000, files_[i].data.size() - src);
        std::memcpy(out.data() + offset_of(blocks[k]), files_[i].data.data() + src, n);
        set_next(out, blocks[k], k + 1 < blocks.size() ? blocks[k + 1] : 0xFFFFFF);
      }
    }
    return out;
  }

  // Backing-block arithmetic for shift 0 (mirrors the reader's; cross-checked against the
  // reference reader in tests).
  static std::uint32_t backing(std::uint32_t block) {
    std::uint32_t adj = 0;
    if (block >= 0xAA) adj += (block / 0xAA) + 1;
    if (block >= 0x70E4) adj += (block / 0x70E4) + 1;
    return block + adj;
  }
  static std::uint64_t offset_of(std::uint32_t block) {
    return 0xC000ull + (static_cast<std::uint64_t>(backing(block)) << 12);
  }

 private:
  static void store_le24(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
  }
  static void set_next(std::vector<std::uint8_t>& out, std::uint32_t block, std::uint32_t next) {
    const std::uint32_t run_first = block - (block % 0xAA);
    const std::uint64_t table = offset_of(run_first) - 0x1000;
    const std::size_t e = static_cast<std::size_t>(table) + (block % 0xAA) * 0x18;
    if (e + 0x18 > out.size()) out.resize(e + 0x18, 0);
    out[e + 0x15] = static_cast<std::uint8_t>(next >> 16);
    out[e + 0x16] = static_cast<std::uint8_t>(next >> 8);
    out[e + 0x17] = static_cast<std::uint8_t>(next);
  }

  std::uint32_t title_id_ = 0x4E4D0859u;
  std::uint32_t content_type_ = 0x000B0000u;
  std::uint32_t version_ = 0x00000302u;
  std::string display_name_ = "Title Update #3";
  std::vector<File> files_;
  bool scramble_ = false;
};

}  // namespace ttt2::test
