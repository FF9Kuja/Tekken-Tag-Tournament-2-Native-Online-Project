#include "ttt2/xex/imports.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

#include "ttt2/core/byte_reader.hpp"
#include "ttt2/core/endian.hpp"
#include "ttt2/core/format.hpp"

namespace ttt2::xex {

#include "../xbox_exports_table.inc"

namespace {

char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool library_is(std::string_view library, std::string_view stem) noexcept {
  // "xboxkrnl.exe" / "xboxkrnl" / "XBOXKRNL.EXE" all name the kernel.
  std::size_t n = library.find('.');
  if (n == std::string_view::npos) n = library.size();
  if (n != stem.size()) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (lower_ascii(library[i]) != stem[i]) return false;
  }
  return true;
}

std::optional<XboxExport> lookup(std::span<const XboxExport> table, std::uint16_t ordinal) noexcept {
  const auto it = std::lower_bound(table.begin(), table.end(), ordinal,
                                   [](const XboxExport& e, std::uint16_t o) { return e.ordinal < o; });
  if (it == table.end() || it->ordinal != ordinal) return std::nullopt;
  return *it;
}

std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct HelperPattern {
  std::string_view name;
  std::array<std::uint8_t, 8> bytes;
  std::size_t length;
};

// First instruction words of the helpers (XenonRecomp README, public): e.g. __restgprlr_14 starts
// with `ld r14, -0x98(r1)` = E9 C1 FF 68; the VMX helpers start with `li r11, -0x120; stvx128 ...`.
constexpr HelperPattern kHelperPatterns[] = {
    {"__restgprlr_14", {0xE9, 0xC1, 0xFF, 0x68}, 4},
    {"__savegprlr_14", {0xF9, 0xC1, 0xFF, 0x68}, 4},
    {"__restfpr_14", {0xC9, 0xCC, 0xFF, 0x70}, 4},
    {"__savefpr_14", {0xD9, 0xCC, 0xFF, 0x70}, 4},
    {"__restvmx_14", {0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE}, 8},
    {"__savevmx_14", {0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE}, 8},
    {"__restvmx_64", {0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB}, 8},
    {"__savevmx_64", {0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB}, 8},
};

}  // namespace

std::optional<XboxExport> find_export(std::string_view library, std::uint16_t ordinal) noexcept {
  if (library_is(library, "xboxkrnl")) return lookup(kXboxkrnlExports, ordinal);
  if (library_is(library, "xam")) return lookup(kXamExports, ordinal);
  return std::nullopt;
}

Expected<std::vector<PeSection>> pe_sections(std::span<const std::uint8_t> image) {
  const ByteReader r(image);
  if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z')
    return make_error("not a PE image (no MZ header)");
  const std::uint32_t nt = le32(image.data() + 0x3C);
  if (!r.in_bounds(nt, 24) || std::memcmp(image.data() + nt, "PE\0\0", 4) != 0) {
    return make_error("not a PE image (no PE signature)");
  }
  const std::uint16_t count = static_cast<std::uint16_t>(image[nt + 6] | (image[nt + 7] << 8));
  const std::uint16_t opt_size = static_cast<std::uint16_t>(image[nt + 20] | (image[nt + 21] << 8));
  const std::size_t table = static_cast<std::size_t>(nt) + 24 + opt_size;
  if (count > 96 || !r.in_bounds(table, static_cast<std::size_t>(count) * 40)) {
    return make_error("PE section table is damaged");
  }
  std::vector<PeSection> sections;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint8_t* h = image.data() + table + static_cast<std::size_t>(i) * 40;
    PeSection s;
    s.name = r.cstring(table + static_cast<std::size_t>(i) * 40, 8).value_or("");
    s.virtual_size = le32(h + 8);
    s.rva = le32(h + 12);
    s.raw_size = le32(h + 16);
    s.characteristics = le32(h + 36);
    sections.push_back(std::move(s));
  }
  return sections;
}

Expected<ImportSummary> resolve_imports(const Xex2Image& xex, std::span<const std::uint8_t> image) {
  if (!xex.security) return make_error("the XEX has no security info (load address unknown)");
  const std::uint32_t base = xex.security->load_address;
  const ByteReader r(image);
  ImportSummary out;
  for (const auto& lib : xex.import_libraries) {
    // Group the records by ordinal: the slot (type 0) and the thunk (type 1) of one import.
    std::map<std::uint16_t, ImportEntry> by_ordinal;
    std::vector<std::uint16_t> order;
    for (const std::uint32_t address : lib.record_addresses) {
      if (address < base || !r.in_bounds(address - base, 4)) {
        out.warnings.push_back(lib.name + ": import record at 0x" + hex_u32(address) +
                               " lies outside the image");
        continue;
      }
      const std::uint32_t word = *r.u32be(address - base);
      const std::uint16_t ordinal = static_cast<std::uint16_t>(word & 0xFFFF);
      const std::uint8_t type = static_cast<std::uint8_t>(word >> 24);
      auto it = by_ordinal.find(ordinal);
      if (it == by_ordinal.end()) {
        ImportEntry e;
        e.library = lib.name;
        e.ordinal = ordinal;
        if (const auto exp = find_export(lib.name, ordinal)) {
          e.name = exp->name;
          e.known_variable = exp->variable;
        }
        it = by_ordinal.emplace(ordinal, std::move(e)).first;
        order.push_back(ordinal);
      }
      if (type == 1) {
        it->second.is_function = true;
        it->second.thunk_address = address;
      } else if (type == 0) {
        it->second.slot_address = address;
      } else {
        out.warnings.push_back(lib.name + ": import record at 0x" + hex_u32(address) + " has unknown type " +
                               std::to_string(type));
      }
    }
    for (const std::uint16_t ordinal : order) {
      ImportEntry& e = by_ordinal[ordinal];
      if (e.is_function) {
        ++out.functions;
      } else {
        ++out.variables;
      }
      if (e.name.empty()) ++out.unnamed;
      out.entries.push_back(std::move(e));
    }
  }
  return out;
}

std::vector<HelperHit> scan_save_restore_helpers(std::span<const std::uint8_t> image,
                                                 std::uint32_t load_address) {
  std::vector<HelperHit> hits;
  for (const auto& p : kHelperPatterns) hits.push_back({p.name, {}});
  const auto sections = pe_sections(image);
  if (!sections) return hits;
  for (const auto& s : *sections) {
    if (!s.executable() || s.virtual_size == 0) continue;
    const std::uint64_t end =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(s.rva) + s.virtual_size, image.size());
    for (std::uint64_t off = s.rva; off + 8 <= end; off += 4) {
      for (std::size_t k = 0; k < std::size(kHelperPatterns); ++k) {
        const auto& p = kHelperPatterns[k];
        if (std::memcmp(image.data() + off, p.bytes.data(), p.length) == 0) {
          hits[k].addresses.push_back(load_address + static_cast<std::uint32_t>(off));
        }
      }
    }
  }
  return hits;
}

}  // namespace ttt2::xex
