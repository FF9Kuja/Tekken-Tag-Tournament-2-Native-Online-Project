// ttt2-validate: checks a Tekken Tag Tournament 2 disc image and title update as a pair, in one
// command, and prints a report that contains only facts (IDs, versions, hashes, counts, addresses).
//
//   ttt2-validate <image.iso> <title-update-file-or-folder> [--json] [--work DIR] [--no-imports]
//
// Steps: find and identify default.xex in the disc image; find the title update (a folder is
// searched for the package whose Default.xexp targets 0.0.3.2, as TTT2Recomp's installer does);
// identify the .xexp; run the clean-room compatibility pre-check (ttt2/xex/patch_check.hpp); with
// the Clang-only patcher compiled in, apply the update in memory and hash the patched image; list
// the kernel/XAM imports and the register save/restore helper addresses of the image the
// recompiler will see. --work DIR also writes base.xex, Default.xexp and default_tu3.xex there (the
// player's own files, for the next pipeline step). Exit code 0 when the pair is usable, 1 otherwise.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/core/project_version.hpp"
#include "ttt2/stfs/stfs.hpp"
#include "ttt2/xdvdfs/xdvdfs.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/imports.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/patch_check.hpp"
#include "ttt2/xex/xex2.hpp"

#if TTT2_HAVE_XEXPATCHER
#include <xex_patcher.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kTtt2TitleId = 0x4E4D0859u;
constexpr std::size_t kMaxExecutableBytes = 64u * 1024 * 1024;

struct Options {
  std::string iso;
  std::string update;
  std::string work;
  bool json = false;
  bool imports = true;
};

/// Collects the report as key/value lines; printed as text or JSON at the end.
class Report {
 public:
  void section(std::string name) { items_.push_back({"", std::move(name), true}); }
  void add(std::string key, std::string value) {
    items_.push_back({std::move(key), std::move(value), false});
  }
  void add(std::string key, std::uint64_t value) { add(std::move(key), std::to_string(value)); }
  void problem(std::string text) { problems_.push_back(std::move(text)); }
  [[nodiscard]] bool ok() const { return problems_.empty(); }

  void print_text() const {
    for (const auto& it : items_) {
      if (it.section) {
        std::printf("\n== %s\n", it.value.c_str());
      } else {
        std::printf("%-26s %s\n", (it.key + ":").c_str(), it.value.c_str());
      }
    }
    std::printf("\n== Verdict\n");
    if (problems_.empty()) {
      std::printf("OK: the disc image and title update form a usable pair\n");
    } else {
      for (const auto& p : problems_) std::printf("PROBLEM: %s\n", p.c_str());
    }
  }

  void print_json() const {
    std::printf("{\n  \"ok\": %s,\n  \"problems\": [", problems_.empty() ? "true" : "false");
    for (std::size_t i = 0; i < problems_.size(); ++i)
      std::printf("%s\"%s\"", i ? ", " : "", escape(problems_[i]).c_str());
    std::printf("],\n  \"report\": {\n");
    std::string section;
    bool first = true;
    for (const auto& it : items_) {
      if (it.section) {
        section = it.value;
        continue;
      }
      std::printf("%s    \"%s\": \"%s\"", first ? "" : ",\n", escape(section + "/" + it.key).c_str(),
                  escape(it.value).c_str());
      first = false;
    }
    std::printf("\n  }\n}\n");
  }

 private:
  struct Item {
    std::string key;
    std::string value;
    bool section;
  };
  static std::string escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
      if (c == '"' || c == '\\') out += '\\';
      if (static_cast<unsigned char>(c) < 0x20) {
        out += ' ';
      } else {
        out += c;
      }
    }
    return out;
  }
  std::vector<Item> items_;
  std::vector<std::string> problems_;
};

std::string sha1_hex(std::span<const std::uint8_t> d) {
  return ttt2::hash::to_hex(ttt2::hash::Sha1::compute(d));
}
std::string sha256_hex(std::span<const std::uint8_t> d) {
  return ttt2::hash::to_hex(ttt2::hash::Sha256::compute(d));
}

void describe_known(Report& rep, const std::string& prefix, const std::string& sha1,
                    const std::string& sha256) {
  if (const auto* known = ttt2::xex::match_known_executable(sha1, sha256)) {
    rep.add(prefix + " matches",
            std::string(known->description) + " (" + std::string(known->provenance) + ")");
  } else {
    rep.add(prefix + " matches", "no known executable (new hash: include it in your report)");
  }
}

/// Reports the facts of a decrypted image the recompiler will see: PE sanity, hashes, imports, helpers.
void describe_image(Report& rep, const std::string& prefix, const ttt2::xex::Xex2Image& xex,
                    std::span<const std::uint8_t> file, bool imports) {
  const auto image = ttt2::xex::load_basefile(xex, file);
  if (!image) {
    rep.add(prefix + " image", "not produced: " + image.error().message);
    return;
  }
  rep.add(prefix + " image",
          std::string(ttt2::xex::looks_like_pe_image(*image) ? "PE image" : "NOT a PE image") + ", " +
              std::to_string(image->size()) + " bytes");
  const std::string sha1 = sha1_hex(*image);
  rep.add(prefix + " image SHA-1", sha1);
  rep.add(prefix + " image SHA-256", sha256_hex(*image));
  describe_known(rep, prefix + " image", sha1, "");
  if (!imports) return;
  if (const auto summary = ttt2::xex::resolve_imports(xex, *image)) {
    rep.add(prefix + " imports", std::to_string(summary->functions) + " functions, " +
                                     std::to_string(summary->variables) + " variables, " +
                                     std::to_string(summary->unnamed) + " unnamed ordinals");
    for (const auto& w : summary->warnings) rep.add(prefix + " import warning", w);
  } else {
    rep.add(prefix + " imports", "not resolved: " + summary.error().message);
  }
  if (xex.security) {
    for (const auto& h : ttt2::xex::scan_save_restore_helpers(*image, xex.security->load_address)) {
      std::string v;
      for (const auto a : h.addresses) v += (v.empty() ? "" : " ") + ("0x" + ttt2::hex_u32(a));
      if (h.addresses.empty()) v = "not found";
      if (h.addresses.size() > 1) v += " (ambiguous)";
      rep.add(prefix + " " + std::string(h.name), v);
    }
  }
}

bool write_file(const fs::path& path, std::span<const std::uint8_t> data) {
  std::ofstream out(path, std::ios::binary);
  return static_cast<bool>(out) && static_cast<bool>(out.write(reinterpret_cast<const char*>(data.data()),
                                                               static_cast<std::streamsize>(data.size())));
}

/// True when the package is a TTT2 title update whose .xexp targets Title Update 3's version.
bool is_ttt2_tu3(ttt2::stfs::Package& pkg) {
  if (pkg.info().title_id != kTtt2TitleId || !pkg.info().is_title_update()) return false;
  const auto entries = pkg.list();
  if (!entries) return false;
  for (const auto& e : *entries) {
    if (e.is_directory || e.name.size() < 5) continue;
    std::string ext = e.name.substr(e.name.size() - 5);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (ext != ".xexp") continue;
    const auto data = pkg.read_file(e, kMaxExecutableBytes);
    if (!data) continue;
    const auto parsed = ttt2::xex::parse(*data);
    if (parsed && parsed->delta_patch &&
        parsed->delta_patch->target_version == ttt2::xex::kTtt2TitleUpdate3Version)
      return true;
  }
  return false;
}

/// Finds the title update package: the file itself, or inside a folder the package identified as TU3.
std::string locate_update(const std::string& given, Report& rep) {
  std::error_code ec;
  if (!fs::is_directory(given, ec)) return given;
  std::size_t scanned = 0;
  for (auto it = fs::recursive_directory_iterator(given, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 4 || !it->is_regular_file(ec)) continue;
    const auto size = it->file_size(ec);
    if (ec || size < 0xC000 || size > 512ull * 1024 * 1024) continue;
    ++scanned;
    auto source = ttt2::FileSource::open(it->path());
    if (!source) continue;
    auto pkg = ttt2::stfs::Package::open(**source);
    if (!pkg) continue;
    if (is_ttt2_tu3(*pkg)) {
      rep.add("Update search", "scanned " + std::to_string(scanned) + " file(s) in " + given + "; picked " +
                                   it->path().string());
      return it->path().string();
    }
  }
  rep.add("Update search", "scanned " + std::to_string(scanned) + " file(s) in " + given +
                               "; no Title Update 3 package found");
  return "";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--version") {
      std::printf("ttt2-validate %s (%s)\n", ttt2::kProjectVersion, ttt2::kProjectGitDescribe);
      return 0;
    } else if (a == "--json") {
      opt.json = true;
    } else if (a == "--no-imports") {
      opt.imports = false;
    } else if (a == "--work" && i + 1 < argc) {
      opt.work = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    } else if (opt.iso.empty()) {
      opt.iso = std::string(a);
    } else if (opt.update.empty()) {
      opt.update = std::string(a);
    }
  }
  if (opt.iso.empty() || opt.update.empty()) {
    std::fprintf(stderr,
                 "usage: ttt2-validate <image.iso> <title-update-file-or-folder> [--json] [--work DIR] "
                 "[--no-imports]\n");
    return 1;
  }
  Report rep;
  rep.section("Tool");
  rep.add("ttt2-validate", std::string(ttt2::kProjectVersion) + " (" + ttt2::kProjectGitDescribe + ")");
#if TTT2_HAVE_XEXPATCHER
  rep.add("Patcher", "compiled in (XenonUtils XexPatcher)");
#else
  rep.add("Patcher",
          "not compiled in (build with the windows-clang-cl or linux-clang preset after "
          "scripts/fetch-xenonrecomp)");
#endif

  // --- disc image -----------------------------------------------------------------------------
  rep.section("Disc image");
  rep.add("File", opt.iso);
  std::vector<std::uint8_t> base_file;
  ttt2::xex::Xex2Image base_xex;
  {
    auto source = ttt2::FileSource::open(opt.iso);
    if (!source) {
      rep.problem("disc image: " + source.error().message);
    } else if (auto disc = ttt2::xdvdfs::Image::open(**source); !disc) {
      rep.problem("disc image: " + disc.error().message);
    } else {
      const auto& v = disc->volume();
      rep.add("Partition offset", "0x" + ttt2::hex_u32(static_cast<std::uint32_t>(v.partition_offset)) +
                                      (v.partition_offset == 0x2080000ull   ? " (XGD3)"
                                       : v.partition_offset == 0xFD90000ull ? " (XGD2)"
                                                                            : ""));
      const auto entry = disc->find("default.xex");
      if (!entry) {
        rep.problem("disc image: no default.xex in the root (" + entry.error().message + ")");
      } else if (auto data = disc->read_file(*entry, kMaxExecutableBytes); !data) {
        rep.problem("disc image: " + data.error().message);
      } else if (auto parsed = ttt2::xex::parse(*data); !parsed) {
        rep.problem("default.xex: " + parsed.error().message);
      } else {
        base_file = std::move(*data);
        base_xex = std::move(*parsed);
        rep.add("default.xex",
                std::to_string(base_file.size()) + " bytes at sector " + std::to_string(entry->start_sector));
        if (base_xex.execution_info) {
          const auto& e = *base_xex.execution_info;
          rep.add("Title ID", ttt2::hex_u32(e.title_id) + (e.title_id == kTtt2TitleId
                                                               ? " (Tekken Tag Tournament 2)"
                                                               : " (NOT Tekken Tag Tournament 2)"));
          rep.add("Version", e.version.to_string() + " (base " + e.base_version.to_string() + ")");
          rep.add("Media ID", ttt2::hex_u32(e.media_id));
          if (e.title_id != kTtt2TitleId)
            rep.problem("the disc image is not Tekken Tag Tournament 2 (title ID " +
                        ttt2::hex_u32(e.title_id) + ")");
          if (e.version != ttt2::xex::kTtt2RetailBaseVersion) {
            rep.problem("the disc executable is version " + e.version.to_string() +
                        ", not the retail 0.0.0.2 that Title Update 3 applies to");
          }
        } else {
          rep.problem("default.xex has no execution info");
        }
        if (base_xex.file_format) {
          rep.add("Encryption", std::string(ttt2::xex::encryption_name(base_xex.file_format->encryption)));
          rep.add("Compression", std::string(ttt2::xex::compression_name(base_xex.file_format->compression)));
        }
        const std::string sha1 = sha1_hex(base_file);
        const std::string sha256 = sha256_hex(base_file);
        rep.add("SHA-1", sha1);
        rep.add("SHA-256", sha256);
        describe_known(rep, "File", sha1, sha256);
        describe_image(rep, "Base", base_xex, base_file, opt.imports);
        for (const auto& w : base_xex.warnings) rep.add("Warning", w);
      }
    }
  }

  // --- title update ---------------------------------------------------------------------------
  rep.section("Title update");
  std::vector<std::uint8_t> patch_file;
  ttt2::xex::Xex2Image patch_xex;
  const std::string update_path = locate_update(opt.update, rep);
  if (update_path.empty()) {
    rep.problem("no Title Update 3 package found at " + opt.update);
  } else {
    rep.add("File", update_path);
    auto source = ttt2::FileSource::open(update_path);
    if (!source) {
      rep.problem("title update: " + source.error().message);
    } else if (auto pkg = ttt2::stfs::Package::open(**source); !pkg) {
      rep.problem("title update: " + pkg.error().message);
    } else {
      const auto& i = pkg->info();
      rep.add("Package title ID", ttt2::hex_u32(i.title_id));
      rep.add("Content type",
              "0x" + ttt2::hex_u32(i.content_type) + (i.is_title_update() ? " (title update)" : ""));
      rep.add("Display name", i.display_name);
      if (i.title_id != kTtt2TitleId || !i.is_title_update())
        rep.problem("the package is not a Tekken Tag Tournament 2 title update");
      const auto entry = pkg->find("Default.xexp");
      if (!entry) {
        rep.problem("title update: no Default.xexp in the package");
      } else if (auto data = pkg->read_file(*entry, kMaxExecutableBytes); !data) {
        rep.problem("title update: " + data.error().message);
      } else if (auto parsed = ttt2::xex::parse(*data); !parsed) {
        rep.problem("Default.xexp: " + parsed.error().message);
      } else {
        patch_file = std::move(*data);
        patch_xex = std::move(*parsed);
        rep.add("Default.xexp", std::to_string(patch_file.size()) + " bytes");
        if (patch_xex.delta_patch) {
          rep.add("Patch versions", patch_xex.delta_patch->source_version.to_string() + " -> " +
                                        patch_xex.delta_patch->target_version.to_string());
          if (patch_xex.delta_patch->target_version != ttt2::xex::kTtt2TitleUpdate3Version) {
            rep.problem("the update targets version " + patch_xex.delta_patch->target_version.to_string() +
                        ", not Title Update 3 (0.0.3.2)");
          }
        } else {
          rep.problem("Default.xexp has no delta patch descriptor");
        }
        rep.add("Patch SHA-1", sha1_hex(patch_file));
        rep.add("Patch SHA-256", sha256_hex(patch_file));
        for (const auto& w : patch_xex.warnings) rep.add("Warning", w);
      }
    }
  }

  // --- the pair -------------------------------------------------------------------------------
  rep.section("Compatibility");
  if (!base_file.empty() && !patch_file.empty()) {
    const auto compat = ttt2::xex::check_patch_compatibility(base_xex, base_file, patch_xex);
    for (const auto& n : compat.notes) rep.add("Note", n);
    for (const auto& e : compat.errors) rep.problem(e);
    rep.add("Pre-check", compat.compatible ? "passed" : "FAILED");
#if TTT2_HAVE_XEXPATCHER
    if (compat.compatible) {
      std::vector<std::uint8_t> patched;
      const auto result = XexPatcher::apply(base_file.data(), base_file.size(), patch_file.data(),
                                            patch_file.size(), patched, false);
      if (result != XexPatcher::Result::Success) {
        rep.problem("applying the title update failed (XexPatcher result " +
                    std::to_string(static_cast<int>(result)) + ")");
      } else if (auto out = ttt2::xex::parse(patched); !out) {
        rep.problem("the patched executable does not parse: " + out.error().message);
      } else {
        rep.add("Patched executable",
                std::to_string(patched.size()) + " bytes, version " +
                    (out->execution_info ? out->execution_info->version.to_string() : "?"));
        rep.add("Patched SHA-1", sha1_hex(patched));
        describe_image(rep, "Patched", *out, patched, opt.imports);
        if (!opt.work.empty()) {
          std::error_code ec;
          fs::create_directories(opt.work, ec);
          const bool ok = write_file(fs::path(opt.work) / "base.xex", base_file) &&
                          write_file(fs::path(opt.work) / "Default.xexp", patch_file) &&
                          write_file(fs::path(opt.work) / "default_tu3.xex", patched);
          rep.add("Work folder",
                  ok ? opt.work + " (base.xex, Default.xexp, default_tu3.xex written; keep it private)"
                     : "could not write to " + opt.work);
          if (!ok) rep.problem("could not write the work folder " + opt.work);
        }
      }
    }
#else
    if (!opt.work.empty()) {
      std::error_code ec;
      fs::create_directories(opt.work, ec);
      const bool ok = write_file(fs::path(opt.work) / "base.xex", base_file) &&
                      write_file(fs::path(opt.work) / "Default.xexp", patch_file);
      rep.add("Work folder",
              ok ? opt.work + " (base.xex, Default.xexp written; run ttt2-xexpatch for default_tu3.xex)"
                 : "could not write to " + opt.work);
    }
#endif
  } else {
    rep.add("Pre-check", "skipped (inputs missing)");
  }

  if (opt.json) {
    rep.print_json();
  } else {
    rep.print_text();
  }
  return rep.ok() ? 0 : 1;
}
