// Kernel / XAM import resolution and executable-image scans for diagnostics.
//
// An Xbox 360 executable imports kernel (xboxkrnl.exe) and XAM (xam.xex) functions by ordinal.
// The XEX import table gives the guest address of every import record; the record word in the
// (decrypted) image holds the ordinal and the record type (address slot or code thunk). Naming
// those ordinals tells us which system calls the game relies on - the list the native runtime has
// to provide (Milestone 4) - and lets the owner's report say "imports resolved: N of M" without
// containing anything but names and addresses.
//
// The save/restore helper scan finds the compiler's register spill helpers by their first
// instruction words (patterns documented in XenonRecomp's README), which every recompiler for this
// platform keys on; reporting their addresses lets a copy be cross-checked against other projects'
// published values (docs/RESEARCH.md). Clean-room code from public documentation.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/expected.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"

namespace ttt2::xex {

struct XboxExport {
  std::uint16_t ordinal;
  const char* name;
  bool variable;  ///< data export (the game imports an address), not a function
};

/// Looks an export up by library ("xboxkrnl.exe" or "xam.xex", case-insensitive, with or without
/// the extension) and ordinal.
[[nodiscard]] std::optional<XboxExport> find_export(std::string_view library, std::uint16_t ordinal) noexcept;

/// What the pinned ReXGlue SDK runtime provides for an import (`src/xex/rexglue_runtime_exports.inc`,
/// generated from the SDK sources by `scripts/gen_runtime_exports.py`). Generated code calls kernel
/// functions directly by name, so a function the runtime neither implements nor stubs does not even
/// link; a stub logs "<name> STUB" and returns; a variable the runtime does not map reads a marker.
enum class RuntimeSupport {
  kUnknown,      ///< the ordinal is not in the export tables, nothing can be said
  kImplemented,  ///< a real implementation, or a variable mapped into guest memory
  kStub,         ///< a logging stub (REX_EXPORT_STUB / REX_EXPORT_STUB_RETURN)
  kMissing,      ///< nothing at all in the runtime
};

/// "unknown", "implemented", "stub" or "missing".
[[nodiscard]] const char* runtime_support_name(RuntimeSupport s) noexcept;
/// Support for a function export, by name.
[[nodiscard]] RuntimeSupport runtime_function_support(std::string_view name) noexcept;
/// Support for a variable export of `library` ("xboxkrnl.exe", case-insensitive) and ordinal.
[[nodiscard]] RuntimeSupport runtime_variable_support(std::string_view library,
                                                      std::uint16_t ordinal) noexcept;
/// The SDK commit the runtime tables were generated from.
[[nodiscard]] std::string_view runtime_exports_sdk_commit() noexcept;

struct ImportEntry {
  std::string library;
  std::uint16_t ordinal = 0;
  bool is_function = false;         ///< a code thunk record exists (type 1)
  std::uint32_t slot_address = 0;   ///< type-0 record: where the kernel writes the address
  std::uint32_t thunk_address = 0;  ///< type-1 record: the 16-byte call thunk (0 for variables)
  std::string name;                 ///< export name, empty if the ordinal is unknown
  bool known_variable = false;      ///< the export table says this is a data export
  RuntimeSupport runtime = RuntimeSupport::kUnknown;  ///< what the pinned SDK runtime provides
};

struct ImportSummary {
  std::vector<ImportEntry> entries;  ///< one per (library, ordinal), in table order
  std::size_t functions = 0;
  std::size_t variables = 0;
  std::size_t unnamed = 0;              ///< ordinals missing from the export tables
  std::size_t runtime_implemented = 0;  ///< named imports the pinned SDK runtime implements
  std::size_t runtime_stubbed = 0;      ///< named imports that reach a logging stub
  std::size_t runtime_missing = 0;      ///< named imports the runtime lacks entirely
  std::vector<std::string> warnings;
};

/// Resolves the import table of a parsed XEX against its decrypted, decompressed image
/// (load_basefile output). Records outside the image are reported as warnings, not errors.
[[nodiscard]] Expected<ImportSummary> resolve_imports(const Xex2Image& xex,
                                                      std::span<const std::uint8_t> image);

struct PeSection {
  std::string name;
  std::uint32_t rva = 0;
  std::uint32_t virtual_size = 0;
  std::uint32_t raw_size = 0;
  std::uint32_t characteristics = 0;

  [[nodiscard]] bool executable() const noexcept { return (characteristics & 0x20000020u) != 0; }
};

/// The section table of a PE32 image (little-endian headers, as on the Xbox 360).
[[nodiscard]] Expected<std::vector<PeSection>> pe_sections(std::span<const std::uint8_t> image);

struct HelperHit {
  std::string_view name;                 ///< e.g. "__savegprlr_14"
  std::vector<std::uint32_t> addresses;  ///< guest addresses where the pattern starts (ideally exactly one)
};

/// Scans the executable sections of a decrypted image for the eight MSVC register save/restore
/// helpers (savegprlr/restgprlr/savefpr/restfpr/savevmx/restvmx 14, savevmx/restvmx 64).
[[nodiscard]] std::vector<HelperHit> scan_save_restore_helpers(std::span<const std::uint8_t> image,
                                                               std::uint32_t load_address);

/// How many of the eight scanned helpers were found exactly once at the address a published set
/// (known_titles.hpp) gives; 8 means the image has that project's layout and its other addresses
/// (function boundaries, switch tables) apply to it.
[[nodiscard]] std::size_t count_published_helper_matches(const std::vector<HelperHit>& hits,
                                                         const PublishedHelpers& set) noexcept;

}  // namespace ttt2::xex
