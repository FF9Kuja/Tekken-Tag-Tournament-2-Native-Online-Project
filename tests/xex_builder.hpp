// Builds synthetic XEX2 container images in memory for tests. No game data is
// involved: every byte is produced here.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ttt2/core/endian.hpp"

namespace ttt2::test {

class XexBuilder {
 public:
  struct Entry {
    std::uint32_t key;
    bool is_inline;
    std::uint32_t inline_value;
    std::vector<std::uint8_t> payload;  // for offset entries (includes the size prefix if 0xFF)
  };

  void add_inline(std::uint32_t key, std::uint32_t value) { entries_.push_back({key, true, value, {}}); }

  void add_struct(std::uint32_t key, std::vector<std::uint8_t> payload) {
    entries_.push_back({key, false, 0, std::move(payload)});
  }

  /// Size-prefixed structure (key low byte 0xFF): the size dword is prepended.
  void add_sized(std::uint32_t key, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> payload(4);
    store_be32(payload.data(), static_cast<std::uint32_t>(body.size() + 4));
    payload.insert(payload.end(), body.begin(), body.end());
    add_struct(key, std::move(payload));
  }

  void set_module_flags(std::uint32_t f) { module_flags_ = f; }
  void set_security(std::vector<std::uint8_t> security) { security_ = std::move(security); }
  void set_basefile(std::vector<std::uint8_t> data) { basefile_ = std::move(data); }

  std::vector<std::uint8_t> build() const {
    const std::size_t table_end = 0x18 + entries_.size() * 8;
    std::vector<std::uint8_t> out(table_end, 0);
    // Lay out payloads after the table, each 4-byte aligned.
    std::vector<std::uint32_t> offsets(entries_.size(), 0);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].is_inline) continue;
      while (out.size() % 4) out.push_back(0);
      offsets[i] = static_cast<std::uint32_t>(out.size());
      out.insert(out.end(), entries_[i].payload.begin(), entries_[i].payload.end());
    }
    while (out.size() % 4) out.push_back(0);
    const std::uint32_t security_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), security_.begin(), security_.end());
    while (out.size() % 0x10) out.push_back(0);
    const std::uint32_t header_size = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), basefile_.begin(), basefile_.end());

    store_be32(out.data() + 0x00, 0x58455832u);  // XEX2
    store_be32(out.data() + 0x04, module_flags_);
    store_be32(out.data() + 0x08, header_size);
    store_be32(out.data() + 0x0C, 0);
    store_be32(out.data() + 0x10, security_offset);
    store_be32(out.data() + 0x14, static_cast<std::uint32_t>(entries_.size()));
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      store_be32(out.data() + 0x18 + i * 8, entries_[i].key);
      store_be32(out.data() + 0x1C + i * 8, entries_[i].is_inline ? entries_[i].inline_value : offsets[i]);
    }
    return out;
  }

  // --- helpers to build common structures ---

  static std::vector<std::uint8_t> execution_info(std::uint32_t media_id, std::uint32_t version,
                                                  std::uint32_t base_version, std::uint32_t title_id,
                                                  std::uint8_t disc_number, std::uint8_t disc_count,
                                                  std::uint32_t savegame_id) {
    std::vector<std::uint8_t> b(0x18, 0);
    store_be32(b.data() + 0x0, media_id);
    store_be32(b.data() + 0x4, version);
    store_be32(b.data() + 0x8, base_version);
    store_be32(b.data() + 0xC, title_id);
    b[0x10] = 0xFF;  // platform
    b[0x11] = 0x02;  // executable table
    b[0x12] = disc_number;
    b[0x13] = disc_count;
    store_be32(b.data() + 0x14, savegame_id);
    return b;
  }

  /// Body of a FILE_FORMAT_INFO header (without the size prefix).
  static std::vector<std::uint8_t> file_format_basic(
      std::uint16_t encryption, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& blocks) {
    std::vector<std::uint8_t> b(4, 0);
    store_be16(b.data(), encryption);
    store_be16(b.data() + 2, 1);  // basic
    for (const auto& [data_size, zero_size] : blocks) {
      std::uint8_t e[8];
      store_be32(e, data_size);
      store_be32(e + 4, zero_size);
      b.insert(b.end(), e, e + 8);
    }
    return b;
  }

  /// Body of a FILE_FORMAT_INFO header for normal (LZX) or delta compression: window size and
  /// one zero block record (without the size prefix).
  static std::vector<std::uint8_t> file_format_normal(std::uint16_t encryption, std::uint16_t compression,
                                                      std::uint32_t window_size) {
    std::vector<std::uint8_t> b(4 + 4 + 24, 0);
    store_be16(b.data(), encryption);
    store_be16(b.data() + 2, compression);
    store_be32(b.data() + 4, window_size);
    return b;
  }

  /// Body of a DELTA_PATCH_DESCRIPTOR (0x4C bytes with the size prefix, so 0x48 here) whose
  /// header window copies `headers_source_size` bytes from `headers_source_offset` of the base.
  struct DeltaDescriptor {
    std::uint32_t target_version;
    std::uint32_t source_version;
    std::uint32_t size_of_target_headers;
    std::uint32_t headers_source_offset;
    std::uint32_t headers_source_size;
    std::uint32_t headers_target_offset;
  };
  static std::vector<std::uint8_t> delta_descriptor(const DeltaDescriptor& d,
                                                    const std::uint8_t* source_digest = nullptr) {
    std::vector<std::uint8_t> b(0x48, 0);
    store_be32(b.data() + 0x0, d.target_version);
    store_be32(b.data() + 0x4, d.source_version);
    if (source_digest) std::memcpy(b.data() + 0x8, source_digest, 0x14);
    std::memset(b.data() + 0x1C, 0x99, 0x10);  // image key source
    store_be32(b.data() + 0x2C, d.size_of_target_headers);
    store_be32(b.data() + 0x30, d.headers_source_offset);
    store_be32(b.data() + 0x34, d.headers_source_size);
    store_be32(b.data() + 0x38, d.headers_target_offset);
    return b;
  }

  /// Body of an IMPORT_LIBRARIES header (without the size prefix).
  struct ImportLib {
    std::string name;
    std::uint32_t id;
    std::uint32_t version;
    std::uint32_t version_min;
    std::uint16_t import_count;
    std::vector<std::uint32_t> records{};  ///< explicit record addresses (else 0x82000000 + 4k)
  };
  static std::vector<std::uint8_t> import_libraries(const std::vector<ImportLib>& libs) {
    std::vector<std::uint8_t> strings;
    for (const auto& l : libs) {
      strings.insert(strings.end(), l.name.begin(), l.name.end());
      strings.push_back(0);
      while (strings.size() % 4) strings.push_back(0);
    }
    std::vector<std::uint8_t> b(8, 0);
    store_be32(b.data(), static_cast<std::uint32_t>(strings.size()));
    store_be32(b.data() + 4, static_cast<std::uint32_t>(libs.size()));
    b.insert(b.end(), strings.begin(), strings.end());
    for (std::size_t i = 0; i < libs.size(); ++i) {
      const auto& l = libs[i];
      const std::uint16_t count =
          l.records.empty() ? l.import_count : static_cast<std::uint16_t>(l.records.size());
      std::vector<std::uint8_t> e(0x28 + static_cast<std::size_t>(count) * 4, 0);
      store_be32(e.data(), static_cast<std::uint32_t>(e.size()));
      std::memset(e.data() + 4, 0xDD, 0x14);  // next import digest
      store_be32(e.data() + 0x18, l.id);
      store_be32(e.data() + 0x1C, l.version);
      store_be32(e.data() + 0x20, l.version_min);
      store_be16(e.data() + 0x24, static_cast<std::uint16_t>(i));
      store_be16(e.data() + 0x26, count);
      for (std::uint16_t k = 0; k < count; ++k)
        store_be32(e.data() + 0x28 + k * 4, l.records.empty() ? 0x82000000u + k * 4u : l.records[k]);
      b.insert(b.end(), e.begin(), e.end());
    }
    return b;
  }

  /// Body of a STATIC_LIBRARIES header (without the size prefix).
  static std::vector<std::uint8_t> static_libraries(
      const std::vector<std::tuple<std::string, std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t>>&
          libs) {
    std::vector<std::uint8_t> b;
    for (const auto& [name, major, minor, build, qfe] : libs) {
      std::uint8_t e[0x10] = {};
      std::memcpy(e, name.data(), name.size() < 8 ? name.size() : 8);
      store_be16(e + 0x8, major);
      store_be16(e + 0xA, minor);
      store_be16(e + 0xC, build);
      e[0xE] = 0;  // approval
      e[0xF] = qfe;
      b.insert(b.end(), e, e + 0x10);
    }
    return b;
  }

  static std::vector<std::uint8_t> sized_string(const std::string& s) {
    std::vector<std::uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    while (b.size() % 4) b.push_back(0);
    return b;
  }

  static std::vector<std::uint8_t> security_info(
      std::uint32_t image_size, std::uint32_t image_flags, std::uint32_t load_address, std::uint32_t region,
      std::uint32_t media_types, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pages,
      const std::uint8_t* file_key = nullptr) {
    std::vector<std::uint8_t> s(0x184 + pages.size() * 0x18, 0);
    store_be32(s.data() + 0x000, static_cast<std::uint32_t>(s.size()));
    store_be32(s.data() + 0x004, image_size);
    std::memset(s.data() + 0x008, 0x11, 0x100);  // "signature"
    store_be32(s.data() + 0x10C, image_flags);
    store_be32(s.data() + 0x110, load_address);
    std::memset(s.data() + 0x114, 0x22, 0x14);  // section digest
    store_be32(s.data() + 0x128, 2);
    std::memset(s.data() + 0x12C, 0x33, 0x14);  // import table digest
    std::memset(s.data() + 0x140, 0x44, 0x10);  // xgd2 media id
    if (file_key) {
      std::memcpy(s.data() + 0x150, file_key, 0x10);
    } else {
      std::memset(s.data() + 0x150, 0x55, 0x10);  // file key
    }
    store_be32(s.data() + 0x160, 0);
    std::memset(s.data() + 0x164, 0x66, 0x14);  // header digest
    store_be32(s.data() + 0x178, region);
    store_be32(s.data() + 0x17C, media_types);
    store_be32(s.data() + 0x180, static_cast<std::uint32_t>(pages.size()));
    for (std::size_t i = 0; i < pages.size(); ++i) {
      const auto& [info, count] = pages[i];
      store_be32(s.data() + 0x184 + i * 0x18, (count << 4) | (info & 0xF));
      std::memset(s.data() + 0x188 + i * 0x18, static_cast<int>(0x70 + i), 0x14);
    }
    return s;
  }

 private:
  std::uint32_t module_flags_ = 0x01;  // TITLE
  std::vector<Entry> entries_;
  std::vector<std::uint8_t> security_;
  std::vector<std::uint8_t> basefile_;
};

}  // namespace ttt2::test
