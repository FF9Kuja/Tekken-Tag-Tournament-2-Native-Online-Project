#include "ttt2/xex/xex2.hpp"

#include <algorithm>
#include <unordered_set>

#include "ttt2/core/byte_reader.hpp"
#include "ttt2/core/file_io.hpp"
#include "ttt2/core/format.hpp"

namespace ttt2::xex {

namespace {

constexpr std::size_t kHeaderSize = 0x18;         // fixed part of xex2_header
constexpr std::size_t kOptHeaderEntrySize = 8;    // key + value/offset
constexpr std::size_t kSecurityInfoSize = 0x184;  // up to the page descriptors
constexpr std::size_t kPageDescriptorSize = 0x18;
constexpr std::size_t kImportLibraryHeaderSize = 0x28;
constexpr std::size_t kStaticLibrarySize = 0x10;

template <std::size_t N>
bool copy_bytes(const ByteReader& r, std::size_t offset, std::array<std::uint8_t, N>& out) {
  const auto s = r.bytes(offset, N);
  if (!s) return false;
  std::copy(s->begin(), s->end(), out.begin());
  return true;
}

/// Optional-header structures and the security info must lie in the header
/// area: after the optional-header table and before the basefile begins.
bool in_header_area(const Xex2Image& img, std::size_t table_end, std::size_t off, std::size_t size) {
  return off >= table_end && off < img.header_size && size <= img.header_size - off;
}

/// Splits the import-library string table: NUL-terminated strings, each padded
/// to a 4-byte boundary.
std::vector<std::string> split_string_table(const ByteReader& r, std::size_t offset, std::size_t table_size) {
  std::vector<std::string> names;
  std::size_t pos = 0;
  while (pos < table_size) {
    auto s = r.cstring(offset + pos, table_size - pos);
    if (!s) break;
    const std::size_t consumed = (s->size() + 1 + 3) & ~static_cast<std::size_t>(3);
    names.push_back(std::move(*s));
    pos += consumed;
  }
  return names;
}

void parse_security_info(const ByteReader& r, Xex2Image& img, std::size_t table_end,
                         const ParseOptions& options) {
  const std::size_t off = img.security_offset;
  if (!in_header_area(img, table_end, off, kSecurityInfoSize)) {
    img.warnings.push_back("security info offset 0x" + hex_u32(img.security_offset) +
                           " is not inside the header area");
    return;
  }
  if (!r.in_bounds(off, kSecurityInfoSize)) {
    img.warnings.push_back("security info at offset 0x" + hex_u32(img.security_offset) +
                           " is outside the file");
    return;
  }
  SecurityInfo s;
  s.header_size = *r.u32be(off + 0x000);
  s.image_size = *r.u32be(off + 0x004);
  // 0x008: RSA signature (0x100 bytes), 0x108: unknown length field
  s.image_flags = *r.u32be(off + 0x10C);
  s.load_address = *r.u32be(off + 0x110);
  copy_bytes(r, off + 0x114, s.section_digest);
  s.import_table_count = *r.u32be(off + 0x128);
  copy_bytes(r, off + 0x12C, s.import_table_digest);
  copy_bytes(r, off + 0x140, s.xgd2_media_id);
  copy_bytes(r, off + 0x150, s.file_key);
  s.export_table = *r.u32be(off + 0x160);
  copy_bytes(r, off + 0x164, s.header_digest);
  s.region = *r.u32be(off + 0x178);
  s.allowed_media_types = *r.u32be(off + 0x17C);
  s.page_descriptor_count = *r.u32be(off + 0x180);

  const std::size_t count = std::min<std::size_t>(s.page_descriptor_count, options.max_page_descriptors);
  if (s.page_descriptor_count > options.max_page_descriptors) {
    img.warnings.push_back("page descriptor count " + std::to_string(s.page_descriptor_count) +
                           " exceeds the parser limit; truncated");
  }
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t pd = off + kSecurityInfoSize + i * kPageDescriptorSize;
    if (!in_header_area(img, table_end, pd, kPageDescriptorSize) || !r.in_bounds(pd, kPageDescriptorSize)) {
      img.warnings.push_back("page descriptors run past the end of the header area");
      break;
    }
    PageDescriptor d;
    const std::uint32_t value = *r.u32be(pd);
    d.info = value & 0xF;
    d.page_count = value >> 4;
    copy_bytes(r, pd + 4, d.digest);
    s.page_descriptors.push_back(d);
  }
  img.security = std::move(s);
}

void parse_file_format_info(const ByteReader& r, std::size_t off, std::size_t size, Xex2Image& img,
                            const ParseOptions& options) {
  if (size < 8 || !r.in_bounds(off, 8)) {
    img.warnings.push_back("file format info is too small");
    return;
  }
  FileFormatInfo f;
  f.encryption = static_cast<EncryptionType>(*r.u16be(off + 4));
  f.compression = static_cast<CompressionType>(*r.u16be(off + 6));
  if (f.compression == CompressionType::Basic) {
    const std::size_t declared_blocks = (size - 8) / 8;
    const std::size_t blocks = std::min(declared_blocks, options.max_table_entries);
    if (declared_blocks > options.max_table_entries) {
      img.warnings.push_back("basic compression block table exceeds the parser limit; truncated");
    }
    for (std::size_t i = 0; i < blocks; ++i) {
      const auto data_size = r.u32be(off + 8 + i * 8);
      const auto zero_size = r.u32be(off + 12 + i * 8);
      if (!data_size || !zero_size) {
        img.warnings.push_back("basic compression block table is truncated");
        break;
      }
      f.basic_blocks.push_back({*data_size, *zero_size});
    }
  } else if (f.compression == CompressionType::Normal || f.compression == CompressionType::Delta) {
    f.normal_window_size = r.u32be(off + 8).value_or(0);
    f.normal_first_block_size = r.u32be(off + 12).value_or(0);
  }
  img.file_format = std::move(f);
}

void parse_execution_info(const ByteReader& r, std::size_t off, Xex2Image& img) {
  if (!r.in_bounds(off, 0x18)) {
    img.warnings.push_back("execution info is outside the file");
    return;
  }
  ExecutionInfo e;
  e.media_id = *r.u32be(off + 0x00);
  e.version = Version::from_u32(*r.u32be(off + 0x04));
  e.base_version = Version::from_u32(*r.u32be(off + 0x08));
  e.title_id = *r.u32be(off + 0x0C);
  e.platform = *r.u8(off + 0x10);
  e.executable_table = *r.u8(off + 0x11);
  e.disc_number = *r.u8(off + 0x12);
  e.disc_count = *r.u8(off + 0x13);
  e.savegame_id = *r.u32be(off + 0x14);
  img.execution_info = e;
}

void parse_import_libraries(const ByteReader& r, std::size_t off, std::size_t size, Xex2Image& img,
                            const ParseOptions& options) {
  if (size < 12 || !r.in_bounds(off, size)) {
    img.warnings.push_back("import library table is truncated");
    return;
  }
  const std::uint32_t string_table_size = *r.u32be(off + 4);
  const std::uint32_t string_count = *r.u32be(off + 8);
  if (12 + static_cast<std::size_t>(string_table_size) > size) {
    img.warnings.push_back("import library string table exceeds its container");
    return;
  }
  const std::vector<std::string> names = split_string_table(r, off + 12, string_table_size);
  if (names.size() != string_count) {
    img.warnings.push_back("import library string count mismatch (declared " + std::to_string(string_count) +
                           ", found " + std::to_string(names.size()) + ")");
  }
  std::size_t pos = off + 12 + string_table_size;
  const std::size_t end = off + size;
  while (pos + kImportLibraryHeaderSize <= end) {
    if (img.import_libraries.size() >= options.max_import_libraries) {
      img.warnings.push_back("import library count exceeds the parser limit; truncated");
      break;
    }
    const std::uint32_t lib_size = *r.u32be(pos);
    ImportLibrary lib;
    lib.id = *r.u32be(pos + 0x18);
    lib.version = Version::from_u32(*r.u32be(pos + 0x1C));
    lib.version_min = Version::from_u32(*r.u32be(pos + 0x20));
    const std::uint16_t name_index = *r.u16be(pos + 0x24);
    lib.import_count = *r.u16be(pos + 0x26);
    lib.name = name_index < names.size() ? names[name_index]
                                         : ("<bad name index " + std::to_string(name_index) + ">");
    img.import_libraries.push_back(std::move(lib));
    if (lib_size < kImportLibraryHeaderSize) {
      img.warnings.push_back("import library entry has an invalid size");
      break;
    }
    pos += lib_size;
  }
}

void parse_static_libraries(const ByteReader& r, std::size_t off, std::size_t size, Xex2Image& img,
                            const ParseOptions& options) {
  if (size < 4 || !r.in_bounds(off, size)) {
    img.warnings.push_back("static library table is truncated");
    return;
  }
  const std::size_t declared = (size - 4) / kStaticLibrarySize;
  const std::size_t count = std::min(declared, options.max_table_entries);
  if (declared > options.max_table_entries) {
    img.warnings.push_back("static library table exceeds the parser limit; truncated");
  }
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t e = off + 4 + i * kStaticLibrarySize;
    StaticLibrary lib;
    lib.name = r.cstring(e, 8).value_or("");
    lib.version_major = *r.u16be(e + 0x8);
    lib.version_minor = *r.u16be(e + 0xA);
    lib.version_build = *r.u16be(e + 0xC);
    lib.approval_type = *r.u8(e + 0xE);
    lib.version_qfe = *r.u8(e + 0xF);
    img.static_libraries.push_back(std::move(lib));
  }
}

void parse_delta_patch(const ByteReader& r, std::size_t off, std::size_t size, Xex2Image& img) {
  if (size < 0x34 || !r.in_bounds(off, 0x34)) {
    img.warnings.push_back("delta patch descriptor is truncated");
    return;
  }
  DeltaPatchInfo d;
  d.target_version = Version::from_u32(*r.u32be(off + 0x4));
  d.source_version = Version::from_u32(*r.u32be(off + 0x8));
  copy_bytes(r, off + 0xC, d.source_digest);
  copy_bytes(r, off + 0x20, d.source_image_key);
  d.size_of_target_headers = *r.u32be(off + 0x30);
  img.delta_patch = d;
}

void parse_tls_info(const ByteReader& r, std::size_t off, Xex2Image& img) {
  if (!r.in_bounds(off, 0x10)) {
    img.warnings.push_back("TLS info is outside the file");
    return;
  }
  img.tls_info = TlsInfo{*r.u32be(off), *r.u32be(off + 4), *r.u32be(off + 8), *r.u32be(off + 12)};
}

void parse_sized_string(const ByteReader& r, std::size_t off, std::size_t size,
                        std::optional<std::string>& out, const char* what, Xex2Image& img) {
  if (size < 4 || !r.in_bounds(off, size)) {
    img.warnings.push_back(std::string(what) + " is truncated");
    return;
  }
  out = r.cstring(off + 4, size - 4);
}

}  // namespace

std::string Version::to_string() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build) + "." +
         std::to_string(qfe);
}

std::string StaticLibrary::version_string() const {
  return std::to_string(version_major) + "." + std::to_string(version_minor) + "." +
         std::to_string(version_build) + "." + std::to_string(version_qfe);
}

const OptionalHeader* Xex2Image::find_header(std::uint32_t key) const noexcept {
  for (const auto& h : optional_headers) {
    if (h.key == key) return &h;
  }
  return nullptr;
}

Expected<Xex2Image> parse(std::span<const std::uint8_t> file, const ParseOptions& options) {
  const ByteReader r(file);
  if (r.size() < kHeaderSize) return make_error("file is too small to be an XEX2 image");
  if (*r.u32be(0) != kXex2Magic) return make_error("not an XEX2 image (bad magic)");

  Xex2Image img;
  img.module_flags = *r.u32be(0x4);
  img.header_size = *r.u32be(0x8);
  img.security_offset = *r.u32be(0x10);
  const std::uint32_t header_count = *r.u32be(0x14);

  if (img.header_size < kHeaderSize) {
    return make_error("header size is smaller than the fixed header");
  }
  if (header_count > options.max_optional_headers) {
    return make_error("optional header count " + std::to_string(header_count) + " is implausible");
  }
  const std::size_t table_end = kHeaderSize + static_cast<std::size_t>(header_count) * kOptHeaderEntrySize;
  if (!r.in_bounds(kHeaderSize, static_cast<std::size_t>(header_count) * kOptHeaderEntrySize)) {
    return make_error("optional header table runs past the end of the file");
  }
  if (table_end > img.header_size) {
    return make_error("optional header table extends past the declared header size");
  }
  if (img.header_size > r.size()) {
    img.warnings.push_back("declared header size 0x" + hex_u32(img.header_size) +
                           " exceeds the file size (truncated file?)");
  }

  std::unordered_set<std::uint32_t> seen_keys;
  for (std::uint32_t i = 0; i < header_count; ++i) {
    const std::size_t e = kHeaderSize + i * kOptHeaderEntrySize;
    OptionalHeader h{*r.u32be(e), *r.u32be(e + 4)};
    img.optional_headers.push_back(h);

    if (!seen_keys.insert(h.key).second) {
      img.warnings.push_back("duplicate optional header " + std::string(optional_header_name(h.key)) +
                             " (0x" + hex_u32(h.key) + ") ignored");
      continue;
    }

    if (h.is_inline()) {
      switch (h.key) {
        case header_key::kEntryPoint:
          img.entry_point = h.data;
          break;
        case header_key::kImageBaseAddress:
          img.image_base = h.data;
          break;
        case header_key::kOriginalBaseAddress:
          img.original_base_address = h.data;
          break;
        case header_key::kDefaultStackSize:
          img.default_stack_size = h.data;
          break;
        case header_key::kDefaultHeapSize:
          img.default_heap_size = h.data;
          break;
        case header_key::kDefaultFilesystemCacheSize:
          img.default_filesystem_cache_size = h.data;
          break;
        case header_key::kSystemFlags:
          img.system_flags = h.data;
          break;
        case header_key::kTitleWorkspaceSize:
          img.title_workspace_size = h.data;
          break;
        case header_key::kAdditionalTitleMemory:
          img.additional_title_memory = h.data;
          break;
        default:
          break;
      }
      continue;
    }

    // Offset-based header: resolve its size, check that the structure lies in
    // the header area (between the table and the basefile) and inside the
    // file, then decode the ones we know.
    const std::size_t off = h.data;
    std::size_t size = 0;
    if (h.is_size_prefixed()) {
      const auto declared = in_header_area(img, table_end, off, 4) ? r.u32be(off) : std::nullopt;
      if (!declared) {
        img.warnings.push_back("optional header " + std::string(optional_header_name(h.key)) +
                               " points outside the header area");
        continue;
      }
      size = *declared;
      if (size < 4 || !in_header_area(img, table_end, off, size) || !r.in_bounds(off, size)) {
        img.warnings.push_back("optional header " + std::string(optional_header_name(h.key)) +
                               " has an invalid size");
        continue;
      }
    } else {
      size = static_cast<std::size_t>(h.key & 0xFF) * 4;
      if (!in_header_area(img, table_end, off, size) || !r.in_bounds(off, size)) {
        img.warnings.push_back("optional header " + std::string(optional_header_name(h.key)) +
                               " points outside the header area");
        continue;
      }
    }

    switch (h.key) {
      case header_key::kFileFormatInfo:
        parse_file_format_info(r, off, size, img, options);
        break;
      case header_key::kExecutionInfo:
        parse_execution_info(r, off, img);
        break;
      case header_key::kImportLibraries:
        parse_import_libraries(r, off, size, img, options);
        break;
      case header_key::kStaticLibraries:
        parse_static_libraries(r, off, size, img, options);
        break;
      case header_key::kDeltaPatchDescriptor:
        parse_delta_patch(r, off, size, img);
        break;
      case header_key::kTlsInfo:
        parse_tls_info(r, off, img);
        break;
      case header_key::kOriginalPeName:
        parse_sized_string(r, off, size, img.original_pe_name, "original PE name", img);
        break;
      case header_key::kBoundingPath:
        parse_sized_string(r, off, size, img.bounding_path, "bounding path", img);
        break;
      case header_key::kChecksumTimestamp:
        img.checksum_timestamp = std::make_pair(*r.u32be(off), *r.u32be(off + 4));
        break;
      case header_key::kAlternateTitleIds: {
        const std::size_t declared_ids = (size - 4) / 4;
        const std::size_t n = std::min(declared_ids, options.max_table_entries);
        if (declared_ids > options.max_table_entries) {
          img.warnings.push_back("alternate title ID table exceeds the parser limit; truncated");
        }
        for (std::size_t k = 0; k < n; ++k) {
          img.alternate_title_ids.push_back(*r.u32be(off + 4 + k * 4));
        }
        break;
      }
      default:
        break;
    }
  }

  parse_security_info(r, img, table_end, options);
  return img;
}

Expected<Xex2Image> parse_file(const std::filesystem::path& path, const ParseOptions& options) {
  auto data = read_file(path);
  if (!data) return data.error();
  return parse(data.value(), options);
}

std::string_view optional_header_name(std::uint32_t key) noexcept {
  switch (key) {
    case header_key::kResourceInfo:
      return "RESOURCE_INFO";
    case header_key::kFileFormatInfo:
      return "FILE_FORMAT_INFO";
    case header_key::kDeltaPatchDescriptor:
      return "DELTA_PATCH_DESCRIPTOR";
    case header_key::kBaseReference:
      return "BASE_REFERENCE";
    case header_key::kBoundingPath:
      return "BOUNDING_PATH";
    case header_key::kDeviceId:
      return "DEVICE_ID";
    case header_key::kOriginalBaseAddress:
      return "ORIGINAL_BASE_ADDRESS";
    case header_key::kEntryPoint:
      return "ENTRY_POINT";
    case header_key::kImageBaseAddress:
      return "IMAGE_BASE_ADDRESS";
    case header_key::kImportLibraries:
      return "IMPORT_LIBRARIES";
    case header_key::kChecksumTimestamp:
      return "CHECKSUM_TIMESTAMP";
    case header_key::kEnabledForCallcap:
      return "ENABLED_FOR_CALLCAP";
    case header_key::kEnabledForFastcap:
      return "ENABLED_FOR_FASTCAP";
    case header_key::kOriginalPeName:
      return "ORIGINAL_PE_NAME";
    case header_key::kStaticLibraries:
      return "STATIC_LIBRARIES";
    case header_key::kTlsInfo:
      return "TLS_INFO";
    case header_key::kDefaultStackSize:
      return "DEFAULT_STACK_SIZE";
    case header_key::kDefaultFilesystemCacheSize:
      return "DEFAULT_FILESYSTEM_CACHE_SIZE";
    case header_key::kDefaultHeapSize:
      return "DEFAULT_HEAP_SIZE";
    case header_key::kPageHeapSizeAndFlags:
      return "PAGE_HEAP_SIZE_AND_FLAGS";
    case header_key::kSystemFlags:
      return "SYSTEM_FLAGS";
    case header_key::kExecutionInfo:
      return "EXECUTION_INFO";
    case header_key::kTitleWorkspaceSize:
      return "TITLE_WORKSPACE_SIZE";
    case header_key::kGameRatings:
      return "GAME_RATINGS";
    case header_key::kLanKey:
      return "LAN_KEY";
    case header_key::kXbox360Logo:
      return "XBOX360_LOGO";
    case header_key::kMultidiscMediaIds:
      return "MULTIDISC_MEDIA_IDS";
    case header_key::kAlternateTitleIds:
      return "ALTERNATE_TITLE_IDS";
    case header_key::kAdditionalTitleMemory:
      return "ADDITIONAL_TITLE_MEMORY";
    case header_key::kExportsByName:
      return "EXPORTS_BY_NAME";
    default:
      return "UNKNOWN";
  }
}

std::string describe_module_flags(std::uint32_t flags) {
  struct Entry {
    std::uint32_t bit;
    const char* name;
  };
  static constexpr Entry kEntries[] = {
      {0x01, "TITLE"},        {0x02, "EXPORTS_TO_TITLE"}, {0x04, "SYSTEM_DEBUGGER"}, {0x08, "DLL_MODULE"},
      {0x10, "MODULE_PATCH"}, {0x20, "PATCH_FULL"},       {0x40, "PATCH_DELTA"},     {0x80, "USER_MODE"},
  };
  std::string out;
  for (const auto& e : kEntries) {
    if (flags & e.bit) {
      if (!out.empty()) out += " | ";
      out += e.name;
    }
  }
  if (out.empty()) out = "(none)";
  return out;
}

std::string describe_region(std::uint32_t region) {
  if (region == 0xFFFFFFFFu) return "ALL";
  std::string out;
  const auto add = [&](const char* s) {
    if (!out.empty()) out += " | ";
    out += s;
  };
  if ((region & 0x000000FFu) == 0x000000FFu) add("NTSC-U");
  if (region & 0x00000100u) add("NTSC-J (Japan)");
  if (region & 0x00000200u) add("NTSC-J (China)");
  if (region & 0x0000FC00u) add("NTSC-J (other)");
  if (region & 0x00010000u) add("PAL (AU/NZ)");
  if (region & 0x00FE0000u) add("PAL");
  if (region & 0xFF000000u) add("OTHER");
  if (out.empty()) out = "(none)";
  return out;
}

std::string_view encryption_name(EncryptionType t) noexcept {
  switch (t) {
    case EncryptionType::None:
      return "none";
    case EncryptionType::Normal:
      return "normal (AES-128-CBC, retail key)";
  }
  return "unknown";
}

std::string_view compression_name(CompressionType t) noexcept {
  switch (t) {
    case CompressionType::None:
      return "none";
    case CompressionType::Basic:
      return "basic (zero-run blocks)";
    case CompressionType::Normal:
      return "normal (LZX)";
    case CompressionType::Delta:
      return "delta (LZX patch)";
  }
  return "unknown";
}

}  // namespace ttt2::xex
