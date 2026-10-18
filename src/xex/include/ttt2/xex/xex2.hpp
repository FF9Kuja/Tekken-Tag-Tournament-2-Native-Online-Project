// XEX2 executable header parser.
//
// Xbox 360 executables ("XEX2" files, usually default.xex) wrap a PE image in
// a big-endian container that carries the title ID, version, media ID, import
// libraries, encryption/compression information and more. Only the container
// headers are parsed here; they are stored in plaintext in every retail XEX,
// so this works on an untouched retail file without any decryption.
//
// The parser is written from the publicly documented format (free60.org
// "XEX" documentation, Xenia's xex2_info.h field layouts) and reads strictly
// through a bounds-checked reader: a damaged or hostile file produces an error
// or a warning, never an out-of-bounds access.
//
// Primary uses in this project:
//   * identifying which game / revision / title update a user supplied
//     (Milestone 3: "document compatible executable versions"),
//   * game-file validation in the launcher (Milestone 8),
//   * the input side of the (future) decrypt / decompress / patch tooling.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ttt2/core/expected.hpp"

namespace ttt2::xex {

inline constexpr std::uint32_t kXex2Magic = 0x58455832u;  // "XEX2"

/// Packed XEX version: major(4 bits).minor(4).build(16).qfe(8), e.g. 0.0.3.2.
struct Version {
  std::uint8_t major = 0;
  std::uint8_t minor = 0;
  std::uint16_t build = 0;
  std::uint8_t qfe = 0;

  [[nodiscard]] static constexpr Version from_u32(std::uint32_t v) noexcept {
    return Version{static_cast<std::uint8_t>(v >> 28), static_cast<std::uint8_t>((v >> 24) & 0xF),
                   static_cast<std::uint16_t>((v >> 8) & 0xFFFF), static_cast<std::uint8_t>(v & 0xFF)};
  }
  [[nodiscard]] constexpr std::uint32_t to_u32() const noexcept {
    return (static_cast<std::uint32_t>(major & 0xF) << 28) | (static_cast<std::uint32_t>(minor & 0xF) << 24) |
           (static_cast<std::uint32_t>(build) << 8) | qfe;
  }
  [[nodiscard]] std::string to_string() const;  // "0.0.3.2"

  friend constexpr bool operator==(const Version&, const Version&) noexcept = default;
};

enum class ModuleFlag : std::uint32_t {
  Title = 0x00000001,
  ExportsToTitle = 0x00000002,
  SystemDebugger = 0x00000004,
  DllModule = 0x00000008,
  ModulePatch = 0x00000010,
  PatchFull = 0x00000020,
  PatchDelta = 0x00000040,
  UserMode = 0x00000080,
};

enum class EncryptionType : std::uint16_t { None = 0, Normal = 1 };
enum class CompressionType : std::uint16_t { None = 0, Basic = 1, Normal = 2, Delta = 3 };

/// Optional header keys. The low byte of a key encodes how its data is stored:
/// 0x00 / 0x01 = the 32-bit value is stored inline in the header entry;
/// 0xFF = the entry holds an offset to a structure whose first dword is its
/// size; any other value N = the entry holds an offset to N*4 bytes of data.
namespace header_key {
inline constexpr std::uint32_t kResourceInfo = 0x000002FF;
inline constexpr std::uint32_t kFileFormatInfo = 0x000003FF;
inline constexpr std::uint32_t kDeltaPatchDescriptor = 0x000005FF;
inline constexpr std::uint32_t kBaseReference = 0x00000405;
inline constexpr std::uint32_t kBoundingPath = 0x000080FF;
inline constexpr std::uint32_t kDeviceId = 0x00008105;
inline constexpr std::uint32_t kOriginalBaseAddress = 0x00010001;
inline constexpr std::uint32_t kEntryPoint = 0x00010100;
inline constexpr std::uint32_t kImageBaseAddress = 0x00010201;
inline constexpr std::uint32_t kImportLibraries = 0x000103FF;
inline constexpr std::uint32_t kChecksumTimestamp = 0x00018002;
inline constexpr std::uint32_t kEnabledForCallcap = 0x00018102;
inline constexpr std::uint32_t kEnabledForFastcap = 0x00018200;
inline constexpr std::uint32_t kOriginalPeName = 0x000183FF;
inline constexpr std::uint32_t kStaticLibraries = 0x000200FF;
inline constexpr std::uint32_t kTlsInfo = 0x00020104;
inline constexpr std::uint32_t kDefaultStackSize = 0x00020200;
inline constexpr std::uint32_t kDefaultFilesystemCacheSize = 0x00020301;
inline constexpr std::uint32_t kDefaultHeapSize = 0x00020401;
inline constexpr std::uint32_t kPageHeapSizeAndFlags = 0x00028002;
inline constexpr std::uint32_t kSystemFlags = 0x00030000;
inline constexpr std::uint32_t kExecutionInfo = 0x00040006;
inline constexpr std::uint32_t kTitleWorkspaceSize = 0x00040201;
inline constexpr std::uint32_t kGameRatings = 0x00040310;
inline constexpr std::uint32_t kLanKey = 0x00040404;
inline constexpr std::uint32_t kXbox360Logo = 0x000405FF;
inline constexpr std::uint32_t kMultidiscMediaIds = 0x000406FF;
inline constexpr std::uint32_t kAlternateTitleIds = 0x000407FF;
inline constexpr std::uint32_t kAdditionalTitleMemory = 0x00040801;
inline constexpr std::uint32_t kExportsByName = 0x00E10402;
}  // namespace header_key

struct OptionalHeader {
  std::uint32_t key = 0;
  std::uint32_t data = 0;  ///< inline value, or file offset of the structure

  [[nodiscard]] constexpr bool is_inline() const noexcept { return (key & 0xFF) <= 1; }
  [[nodiscard]] constexpr bool is_size_prefixed() const noexcept { return (key & 0xFF) == 0xFF; }
};

struct BasicCompressionBlock {
  std::uint32_t data_size = 0;
  std::uint32_t zero_size = 0;
};

struct FileFormatInfo {
  EncryptionType encryption = EncryptionType::None;
  CompressionType compression = CompressionType::None;
  std::vector<BasicCompressionBlock> basic_blocks;  ///< CompressionType::Basic
  std::uint32_t normal_window_size = 0;             ///< CompressionType::Normal
  std::uint32_t normal_first_block_size = 0;        ///< CompressionType::Normal
};

struct ExecutionInfo {
  std::uint32_t media_id = 0;
  Version version;
  Version base_version;
  std::uint32_t title_id = 0;
  std::uint8_t platform = 0;
  std::uint8_t executable_table = 0;
  std::uint8_t disc_number = 0;
  std::uint8_t disc_count = 0;
  std::uint32_t savegame_id = 0;
};

struct ImportLibrary {
  std::string name;  ///< e.g. "xboxkrnl.exe", "xam.xex"
  std::uint32_t id = 0;
  Version version;
  Version version_min;
  std::uint16_t import_count = 0;
};

struct StaticLibrary {
  std::string name;  ///< e.g. "XAPILIB", "D3D9", "XAUDIO2"
  std::uint16_t version_major = 0;
  std::uint16_t version_minor = 0;
  std::uint16_t version_build = 0;
  std::uint8_t approval_type = 0;
  std::uint8_t version_qfe = 0;

  [[nodiscard]] std::string version_string() const;  // "2.0.20353.0"
};

struct TlsInfo {
  std::uint32_t slot_count = 0;
  std::uint32_t raw_data_address = 0;
  std::uint32_t data_size = 0;
  std::uint32_t raw_data_size = 0;
};

/// Present in title-update patches (.xexp). Identifies which base image the
/// patch applies to and which version it produces.
struct DeltaPatchInfo {
  Version target_version;
  Version source_version;
  std::array<std::uint8_t, 20> source_digest{};     ///< SHA-1 the base image's security header must match
  std::array<std::uint8_t, 16> source_image_key{};  ///< base image's file key
  std::uint32_t size_of_target_headers = 0;
};

struct PageDescriptor {
  std::uint32_t info = 0;        ///< section type (1 code, 2 data, 3 read-only data)
  std::uint32_t page_count = 0;  ///< pages (4 KiB or 64 KiB per image flags)
  std::array<std::uint8_t, 20> digest{};
};

struct SecurityInfo {
  std::uint32_t header_size = 0;
  std::uint32_t image_size = 0;
  std::uint32_t image_flags = 0;
  std::uint32_t load_address = 0;
  std::array<std::uint8_t, 20> section_digest{};
  std::uint32_t import_table_count = 0;
  std::array<std::uint8_t, 20> import_table_digest{};
  std::array<std::uint8_t, 16> xgd2_media_id{};
  std::array<std::uint8_t, 16> file_key{};  ///< encrypted session key (decrypt with the retail key)
  std::uint32_t export_table = 0;
  std::array<std::uint8_t, 20> header_digest{};
  std::uint32_t region = 0;
  std::uint32_t allowed_media_types = 0;
  std::uint32_t page_descriptor_count = 0;
  std::vector<PageDescriptor> page_descriptors;
};

struct Xex2Image {
  std::uint32_t module_flags = 0;
  std::uint32_t header_size = 0;  ///< file offset where the (encrypted) basefile begins
  std::uint32_t security_offset = 0;
  std::vector<OptionalHeader> optional_headers;

  std::optional<SecurityInfo> security;
  std::optional<ExecutionInfo> execution_info;
  std::optional<FileFormatInfo> file_format;
  std::optional<DeltaPatchInfo> delta_patch;
  std::optional<TlsInfo> tls_info;

  std::optional<std::uint32_t> entry_point;
  std::optional<std::uint32_t> image_base;
  std::optional<std::uint32_t> original_base_address;
  std::optional<std::uint32_t> default_stack_size;
  std::optional<std::uint32_t> default_heap_size;
  std::optional<std::uint32_t> default_filesystem_cache_size;
  std::optional<std::uint32_t> system_flags;
  std::optional<std::uint32_t> title_workspace_size;
  std::optional<std::uint32_t> additional_title_memory;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> checksum_timestamp;  ///< (checksum, time_t)
  std::optional<std::string> original_pe_name;
  std::optional<std::string> bounding_path;
  std::vector<ImportLibrary> import_libraries;
  std::vector<StaticLibrary> static_libraries;
  std::vector<std::uint32_t> alternate_title_ids;

  std::vector<std::string> warnings;  ///< non-fatal oddities found while parsing

  [[nodiscard]] bool has_flag(ModuleFlag f) const noexcept {
    return (module_flags & static_cast<std::uint32_t>(f)) != 0;
  }
  [[nodiscard]] bool is_title() const noexcept { return has_flag(ModuleFlag::Title); }
  [[nodiscard]] bool is_dll() const noexcept { return has_flag(ModuleFlag::DllModule); }
  [[nodiscard]] bool is_patch() const noexcept { return has_flag(ModuleFlag::ModulePatch); }
  [[nodiscard]] bool is_delta_patch() const noexcept { return has_flag(ModuleFlag::PatchDelta); }

  [[nodiscard]] const OptionalHeader* find_header(std::uint32_t key) const noexcept;
};

struct ParseOptions {
  std::size_t max_optional_headers = 1024;
  std::size_t max_page_descriptors = 65536;
  std::size_t max_import_libraries = 256;
};

/// Parses the XEX2 container headers of an in-memory file image.
[[nodiscard]] Expected<Xex2Image> parse(std::span<const std::uint8_t> file, const ParseOptions& options = {});

/// Reads and parses a file. Only the headers are interpreted; the whole file
/// is read so callers can hash it afterwards.
[[nodiscard]] Expected<Xex2Image> parse_file(const std::filesystem::path& path,
                                             const ParseOptions& options = {});

/// Human-readable name of an optional header key ("UNKNOWN" if not recognised).
[[nodiscard]] std::string_view optional_header_name(std::uint32_t key) noexcept;

[[nodiscard]] std::string describe_module_flags(std::uint32_t flags);
[[nodiscard]] std::string describe_region(std::uint32_t region);
[[nodiscard]] std::string_view encryption_name(EncryptionType t) noexcept;
[[nodiscard]] std::string_view compression_name(CompressionType t) noexcept;

}  // namespace ttt2::xex
