// ttt2-xexinfo: prints what an Xbox 360 executable (.xex) or title update
// patch (.xexp) is, without decrypting it.
//
//   ttt2-xexinfo <file> [--json] [--no-hash] [--headers] [--image-hash] [--imports] [--helpers]
//
// --imports and --helpers decrypt and decompress the executable image in memory (like
// --image-hash) and report, respectively, the kernel/XAM imports by name (with what the pinned
// ReXGlue runtime provides for each: implemented, stub or missing) and the addresses of
// the compiler's register save/restore helpers: names, ordinals and addresses only.
// Exit code 0 on success, 1 on a parse error or bad arguments.

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/file_io.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/core/project_version.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/imports.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

struct Options {
  std::string path;
  bool json = false;
  bool hash = true;
  bool headers = false;
  bool image_hash = false;  // decrypt + decompress the basefile in memory and hash it
  bool imports = false;     // resolve the import table against the image
  bool helpers = false;     // scan the image for the register save/restore helpers
};

struct ImageScan {
  std::optional<ttt2::xex::ImportSummary> imports;
  std::vector<ttt2::xex::HelperHit> helpers;
};

void print_usage() {
  std::fprintf(
      stderr,
      "usage: ttt2-xexinfo <file.xex|file.xexp> [--json] [--no-hash] [--headers] [--image-hash]\n"
      "                    [--imports] [--helpers]\n"
      "       ttt2-xexinfo --version\n\n"
      "Reads the plaintext XEX2 container headers of an Xbox 360 executable and\n"
      "reports title ID, version, media ID, imports and encryption/compression.\n"
      "--image-hash decrypts and decompresses the executable image in memory (retail key) and\n"
      "prints its hashes; --imports names every kernel/XAM import (ordinal, thunk and slot\n"
      "addresses); --helpers locates the register save/restore helpers. Nothing is written to disk.\n");
}

std::string json_escape(std::string_view s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        // Control characters and any non-ASCII byte are escaped so the output stays
        // valid JSON even when a damaged file carries arbitrary bytes.
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

struct Hashes {
  std::string sha1;
  std::string sha256;
  std::string image_sha1;  // of the decrypted + decompressed image (--image-hash)
  std::string image_sha256;
  std::string image_note;  // "PE image, N bytes" or the reason it could not be produced
};

void print_text(const ttt2::xex::Xex2Image& img, const Options& opt, std::size_t file_size,
                const Hashes* hashes, const ImageScan* scan) {
  using namespace ttt2::xex;
  std::printf("File:               %s (%zu bytes)\n", opt.path.c_str(), file_size);
  std::printf("Module flags:       0x%08X  %s\n", img.module_flags,
              describe_module_flags(img.module_flags).c_str());
  std::printf("Kind:               %s\n", img.is_delta_patch() ? "title update (delta patch, .xexp)"
                                          : img.is_patch()     ? "patch module"
                                          : img.is_dll()       ? "DLL module"
                                                               : "title executable");
  std::printf("Header size:        0x%08X (basefile starts here)\n", img.header_size);
  if (img.execution_info) {
    const auto& e = *img.execution_info;
    const auto name = known_title_name(e.title_id);
    std::printf("Title ID:           %s%s%s\n", ttt2::hex_u32(e.title_id).c_str(), name ? "  " : "",
                name ? std::string(*name).c_str() : "");
    std::printf("Version:            %s (base %s)\n", e.version.to_string().c_str(),
                e.base_version.to_string().c_str());
    std::printf("Media ID:           %s\n", ttt2::hex_u32(e.media_id).c_str());
    std::printf("Disc:               %u of %u, platform %u, savegame ID %s\n", e.disc_number, e.disc_count,
                e.platform, ttt2::hex_u32(e.savegame_id).c_str());
  }
  if (img.delta_patch) {
    const auto& d = *img.delta_patch;
    std::printf("Patch:              %s -> %s\n", d.source_version.to_string().c_str(),
                d.target_version.to_string().c_str());
    std::printf("Patch source hash:  %s\n", ttt2::hash::to_hex(d.source_digest).c_str());
    if (d.target_version == kTtt2TitleUpdate3Version && img.execution_info &&
        img.execution_info->title_id == 0x4E4D0859u) {
      std::printf("Identified:         Tekken Tag Tournament 2 Title Update 3 (target 0.0.3.2)\n");
    }
  }
  if (img.entry_point) std::printf("Entry point:        0x%08X\n", *img.entry_point);
  if (img.image_base) std::printf("Image base:         0x%08X\n", *img.image_base);
  if (img.original_base_address) std::printf("Original base:      0x%08X\n", *img.original_base_address);
  if (img.default_stack_size) std::printf("Default stack:      0x%08X\n", *img.default_stack_size);
  if (img.default_heap_size) std::printf("Default heap:       0x%08X\n", *img.default_heap_size);
  if (img.system_flags) std::printf("System flags:       0x%08X\n", *img.system_flags);
  if (img.original_pe_name) std::printf("Original PE name:   %s\n", img.original_pe_name->c_str());
  if (img.checksum_timestamp) {
    std::printf("Checksum/time:      0x%08X / 0x%08X\n", img.checksum_timestamp->first,
                img.checksum_timestamp->second);
  }
  if (img.file_format) {
    const auto& f = *img.file_format;
    std::printf("Encryption:         %s\n", std::string(encryption_name(f.encryption)).c_str());
    std::printf("Compression:        %s", std::string(compression_name(f.compression)).c_str());
    if (f.compression == CompressionType::Basic) std::printf(", %zu blocks", f.basic_blocks.size());
    if (f.compression == CompressionType::Normal || f.compression == CompressionType::Delta) {
      std::printf(", window 0x%X", f.normal_window_size);
    }
    std::printf("\n");
  }
  if (img.security) {
    const auto& s = *img.security;
    std::printf("Load address:       0x%08X, image size 0x%08X\n", s.load_address, s.image_size);
    std::printf("Image flags:        0x%08X\n", s.image_flags);
    std::printf("Region:             0x%08X  %s\n", s.region, describe_region(s.region).c_str());
    std::printf("Media types:        0x%08X\n", s.allowed_media_types);
    std::printf("Page descriptors:   %u (%zu parsed)\n", s.page_descriptor_count, s.page_descriptors.size());
    std::printf("Header digest:      %s\n", ttt2::hash::to_hex(s.header_digest).c_str());
  }
  if (!img.import_libraries.empty()) {
    std::printf("Import libraries:\n");
    for (const auto& lib : img.import_libraries) {
      std::printf("  %-16s v%s (min %s), %u imports\n", lib.name.c_str(), lib.version.to_string().c_str(),
                  lib.version_min.to_string().c_str(), lib.import_count);
    }
  }
  if (!img.static_libraries.empty()) {
    std::printf("Static libraries:\n");
    for (const auto& lib : img.static_libraries) {
      std::printf("  %-10s %s\n", lib.name.c_str(), lib.version_string().c_str());
    }
  }
  if (opt.headers) {
    std::printf("Optional headers (%zu):\n", img.optional_headers.size());
    for (const auto& h : img.optional_headers) {
      std::printf("  %08X %-30s %s0x%08X\n", h.key, std::string(optional_header_name(h.key)).c_str(),
                  h.is_inline() ? "value " : "at    ", h.data);
    }
  }
  if (hashes) {
    std::printf("SHA-1:              %s\n", hashes->sha1.c_str());
    std::printf("SHA-256:            %s\n", hashes->sha256.c_str());
    if (const auto* known = match_known_executable(hashes->sha1, hashes->sha256)) {
      std::printf("Matches:            %s\n", std::string(known->description).c_str());
      std::printf("                    (%s)\n", std::string(known->provenance).c_str());
    }
    if (!hashes->image_note.empty()) std::printf("Image:              %s\n", hashes->image_note.c_str());
    if (!hashes->image_sha1.empty()) {
      std::printf("Image SHA-1:        %s\n", hashes->image_sha1.c_str());
      std::printf("Image SHA-256:      %s\n", hashes->image_sha256.c_str());
      if (const auto* known = match_known_executable(hashes->image_sha1, hashes->image_sha256)) {
        std::printf("Image matches:      %s\n", std::string(known->description).c_str());
      }
    }
  }
  if (scan && scan->imports) {
    const auto& im = *scan->imports;
    std::printf("Imports:            %zu functions, %zu variables, %zu unnamed ordinals\n", im.functions,
                im.variables, im.unnamed);
    std::printf(
        "Runtime support:    ReXGlue SDK %s: %zu implemented, %zu stubs, %zu missing (named imports)\n",
        std::string(ttt2::xex::runtime_exports_sdk_commit()).substr(0, 12).c_str(), im.runtime_implemented,
        im.runtime_stubbed, im.runtime_missing);
    std::string current;
    for (const auto& e : im.entries) {
      if (e.library != current) {
        current = e.library;
        std::printf("  %s\n", current.c_str());
      }
      const char* runtime = e.runtime == ttt2::xex::RuntimeSupport::kUnknown ? "-"
                            : e.runtime == ttt2::xex::RuntimeSupport::kMissing
                                ? "MISSING"
                                : ttt2::xex::runtime_support_name(e.runtime);
      std::printf("    %5u  %-40s %-8s %-11s", e.ordinal,
                  e.name.empty() ? "(unknown ordinal)" : e.name.c_str(),
                  e.is_function ? "function" : "variable", runtime);
      if (e.thunk_address) std::printf("  thunk 0x%08X", e.thunk_address);
      if (e.slot_address) std::printf("  slot 0x%08X", e.slot_address);
      std::printf("\n");
    }
    for (const auto& w : im.warnings) std::printf("Warning:            %s\n", w.c_str());
  }
  if (scan && !scan->helpers.empty()) {
    std::printf("Helpers:\n");
    for (const auto& h : scan->helpers) {
      std::printf("  %-16s", std::string(h.name).c_str());
      if (h.addresses.empty()) {
        std::printf(" not found\n");
      } else {
        for (const auto a : h.addresses) std::printf(" 0x%08X", a);
        std::printf("%s\n", h.addresses.size() > 1 ? "  (ambiguous)" : "");
      }
    }
  }
  for (const auto& w : img.warnings) std::printf("Warning:            %s\n", w.c_str());
}

void print_json(const ttt2::xex::Xex2Image& img, const Options& opt, std::size_t file_size,
                const Hashes* hashes, const ImageScan* scan) {
  using namespace ttt2::xex;
  std::printf("{\n");
  std::printf("  \"file\": \"%s\",\n  \"size\": %zu,\n", json_escape(opt.path).c_str(), file_size);
  std::printf("  \"module_flags\": %u,\n  \"is_title\": %s,\n  \"is_delta_patch\": %s,\n", img.module_flags,
              img.is_title() ? "true" : "false", img.is_delta_patch() ? "true" : "false");
  std::printf("  \"header_size\": %u,\n", img.header_size);
  if (img.execution_info) {
    const auto& e = *img.execution_info;
    const auto name = known_title_name(e.title_id);
    std::printf("  \"title_id\": \"%s\",\n  \"title_name\": %s,\n", ttt2::hex_u32(e.title_id).c_str(),
                name ? ("\"" + json_escape(*name) + "\"").c_str() : "null");
    std::printf("  \"version\": \"%s\",\n  \"base_version\": \"%s\",\n  \"media_id\": \"%s\",\n",
                e.version.to_string().c_str(), e.base_version.to_string().c_str(),
                ttt2::hex_u32(e.media_id).c_str());
    std::printf("  \"disc_number\": %u,\n  \"disc_count\": %u,\n", e.disc_number, e.disc_count);
  }
  if (img.delta_patch) {
    std::printf("  \"patch_source_version\": \"%s\",\n  \"patch_target_version\": \"%s\",\n",
                img.delta_patch->source_version.to_string().c_str(),
                img.delta_patch->target_version.to_string().c_str());
  }
  if (img.entry_point) std::printf("  \"entry_point\": \"%s\",\n", ttt2::hex_u32(*img.entry_point).c_str());
  if (img.image_base) std::printf("  \"image_base\": \"%s\",\n", ttt2::hex_u32(*img.image_base).c_str());
  if (img.original_pe_name)
    std::printf("  \"original_pe_name\": \"%s\",\n", json_escape(*img.original_pe_name).c_str());
  if (img.file_format) {
    std::printf("  \"encryption\": %u,\n  \"compression\": %u,\n",
                static_cast<unsigned>(img.file_format->encryption),
                static_cast<unsigned>(img.file_format->compression));
  }
  if (img.security) {
    std::printf("  \"load_address\": \"%s\",\n  \"image_size\": %u,\n  \"region\": \"%s\",\n",
                ttt2::hex_u32(img.security->load_address).c_str(), img.security->image_size,
                ttt2::hex_u32(img.security->region).c_str());
  }
  std::printf("  \"import_libraries\": [");
  for (std::size_t i = 0; i < img.import_libraries.size(); ++i) {
    const auto& lib = img.import_libraries[i];
    std::printf("%s{\"name\": \"%s\", \"version\": \"%s\", \"imports\": %u}", i ? ", " : "",
                json_escape(lib.name).c_str(), lib.version.to_string().c_str(), lib.import_count);
  }
  std::printf("],\n");
  if (hashes) {
    std::printf("  \"sha1\": \"%s\",\n  \"sha256\": \"%s\",\n", hashes->sha1.c_str(), hashes->sha256.c_str());
    const auto* known = match_known_executable(hashes->sha1, hashes->sha256);
    std::printf("  \"known_match\": %s,\n",
                known ? ("\"" + json_escape(known->description) + "\"").c_str() : "null");
    if (!hashes->image_sha1.empty()) {
      std::printf("  \"image_sha1\": \"%s\",\n  \"image_sha256\": \"%s\",\n", hashes->image_sha1.c_str(),
                  hashes->image_sha256.c_str());
    }
    if (!hashes->image_note.empty())
      std::printf("  \"image_note\": \"%s\",\n", json_escape(hashes->image_note).c_str());
  }
  if (scan && scan->imports) {
    const auto& im = *scan->imports;
    std::printf("  \"import_functions\": %zu,\n  \"import_variables\": %zu,\n  \"import_unnamed\": %zu,\n",
                im.functions, im.variables, im.unnamed);
    std::printf(
        "  \"runtime_sdk_commit\": \"%s\",\n  \"runtime_implemented\": %zu,\n  \"runtime_stubbed\": %zu,\n"
        "  \"runtime_missing\": %zu,\n",
        std::string(ttt2::xex::runtime_exports_sdk_commit()).c_str(), im.runtime_implemented,
        im.runtime_stubbed, im.runtime_missing);
    std::printf("  \"imports\": [");
    for (std::size_t i = 0; i < im.entries.size(); ++i) {
      const auto& e = im.entries[i];
      const std::string name = e.name.empty() ? "null" : ("\"" + json_escape(e.name) + "\"");
      const std::string runtime =
          e.runtime == ttt2::xex::RuntimeSupport::kUnknown
              ? "null"
              : ("\"" + std::string(ttt2::xex::runtime_support_name(e.runtime)) + "\"");
      std::printf(
          "%s\n    {\"library\": \"%s\", \"ordinal\": %u, \"name\": %s, \"function\": %s, "
          "\"runtime\": %s, \"thunk\": \"%s\", \"slot\": \"%s\"}",
          i ? "," : "", json_escape(e.library).c_str(), e.ordinal, name.c_str(),
          e.is_function ? "true" : "false", runtime.c_str(), ttt2::hex_u32(e.thunk_address).c_str(),
          ttt2::hex_u32(e.slot_address).c_str());
    }
    std::printf("\n  ],\n");
  }
  if (scan && !scan->helpers.empty()) {
    std::printf("  \"helpers\": {");
    for (std::size_t i = 0; i < scan->helpers.size(); ++i) {
      const auto& h = scan->helpers[i];
      std::printf("%s\"%s\": [", i ? ", " : "", std::string(h.name).c_str());
      for (std::size_t k = 0; k < h.addresses.size(); ++k) {
        std::printf("%s\"%s\"", k ? ", " : "", ttt2::hex_u32(h.addresses[k]).c_str());
      }
      std::printf("]");
    }
    std::printf("},\n");
  }
  std::printf("  \"warnings\": [");
  for (std::size_t i = 0; i < img.warnings.size(); ++i) {
    std::printf("%s\"%s\"", i ? ", " : "", json_escape(img.warnings[i]).c_str());
  }
  std::printf("]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--json") {
      opt.json = true;
    } else if (a == "--no-hash") {
      opt.hash = false;
    } else if (a == "--headers") {
      opt.headers = true;
    } else if (a == "--image-hash") {
      opt.image_hash = true;
    } else if (a == "--imports") {
      opt.imports = true;
    } else if (a == "--helpers") {
      opt.helpers = true;
    } else if (a == "--version") {
      std::printf("ttt2-xexinfo %s (%s)\n", ttt2::kProjectVersion, ttt2::kProjectGitDescribe);
      return 0;
    } else if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      print_usage();
      return 1;
    } else if (opt.path.empty()) {
      opt.path = std::string(a);
    } else {
      std::fprintf(stderr, "only one file may be given\n");
      return 1;
    }
  }
  if (opt.path.empty()) {
    print_usage();
    return 1;
  }

  auto data = ttt2::read_file(opt.path);
  if (!data) {
    std::fprintf(stderr, "error: %s\n", data.error().message.c_str());
    return 1;
  }
  auto parsed = ttt2::xex::parse(data.value());
  if (!parsed) {
    std::fprintf(stderr, "error: %s\n", parsed.error().message.c_str());
    return 1;
  }

  Hashes hashes;
  if (opt.hash) {
    hashes.sha1 = ttt2::hash::to_hex(ttt2::hash::Sha1::compute(data.value()));
    hashes.sha256 = ttt2::hash::to_hex(ttt2::hash::Sha256::compute(data.value()));
  }
  ImageScan scan;
  if (opt.image_hash || opt.imports || opt.helpers) {
    opt.hash = true;
    auto image = ttt2::xex::load_basefile(parsed.value(), data.value());
    if (!image) {
      hashes.image_note = "not produced: " + image.error().message;
    } else {
      if (opt.image_hash) {
        hashes.image_sha1 = ttt2::hash::to_hex(ttt2::hash::Sha1::compute(image.value()));
        hashes.image_sha256 = ttt2::hash::to_hex(ttt2::hash::Sha256::compute(image.value()));
      }
      hashes.image_note =
          (ttt2::xex::looks_like_pe_image(image.value()) ? "PE image, "
                                                         : "NOT a PE image (wrong key or damaged file), ") +
          std::to_string(image->size()) + " bytes";
      if (opt.imports) {
        auto summary = ttt2::xex::resolve_imports(parsed.value(), image.value());
        if (summary) {
          scan.imports = std::move(summary.value());
        } else {
          parsed->warnings.push_back("imports: " + summary.error().message);
        }
      }
      if (opt.helpers && parsed->security) {
        scan.helpers = ttt2::xex::scan_save_restore_helpers(image.value(), parsed->security->load_address);
      }
    }
  }
  if (opt.json) {
    print_json(parsed.value(), opt, data->size(), opt.hash ? &hashes : nullptr, &scan);
  } else {
    print_text(parsed.value(), opt, data->size(), opt.hash ? &hashes : nullptr, &scan);
  }
  return 0;
}
